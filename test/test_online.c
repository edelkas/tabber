/*
 * test_online.c - The tests that need the live digest and mappack server.
 *
 * Skipped unless --online is passed, since they depend on the network and on
 * what is published at the time. --full adds the sweep over every tab, which
 * downloads and verifies the whole catalogue.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "digest.h"
#include "json.h"
#include "platform.h"
#include "server.h"
#include "tabs.h"
#include "test.h"
#include "util.h"

/* The catalogue only ever grows, so this is a floor, not an equality. */
#define MIN_PUBLISHED_TABS  24

/* A small tab, used for the single-download test. */
#define SAMPLE_CODE         "lit"

static void test_digest_download(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("online_digest");
    char *path;
    digest *dig;
    const npp_tab *met;

    test_case("fetching the live digest");
    test_use_root(root);

    CHECK(digest_ensure_fresh(1, err, sizeof err) == 0, "the digest downloads (%s)", err);
    path = digest_cache_path();
    CHECK(plat_is_file(path), "it is cached next to the tool");

    dig = digest_load(err, sizeof err);
    CHECK(dig != NULL, "the cached digest parses (%s)", dig ? "" : err);
    if (dig) {
        CHECK(dig->tab_count >= MIN_PUBLISHED_TABS,
              "it lists at least %d tabs (got %u)", MIN_PUBLISHED_TABS,
              (unsigned)dig->tab_count);
        CHECK(dig->signature_date != NULL, "it carries a signature date");
        CHECK_STR(digest_levels_dir(dig), "Levels", "the levels folder name");

        met = digest_find(dig, "MET");
        CHECK(met != NULL, "'met' is published and found case-insensitively");
        if (met) {
            CHECK_NUM(met->id, 0, "met is tab 0");
            CHECK(met->name && *met->name, "it has a name");
            CHECK(json_get(met->node, "download") != NULL, "it has a download block");
        }
        digest_free(dig);
    }

    /* A second call in the same session is a no-op that still succeeds. */
    CHECK(digest_ensure_fresh(0, err, sizeof err) == 0, "the session refresh is a no-op");

    free(path);
    free(root);
}

/* Downloads one tab and puts every check the fetch makes to work. */
static int fetch_and_verify(const digest *dig, const npp_tab *tab, int verbose)
{
    char err[TB_ERR_LEN];
    tab_report report;
    const json_value *download;
    const char *expected_md5;
    int ok;

    if (tab_fetch(dig, tab, &report, err, sizeof err) != 0) {
        CHECK(0, "'%s' fetches (%s)", tab->code, err);
        return 0;
    }

    download = json_get(tab->node, "download");
    expected_md5 = json_get_string(download, "md5", "");
    ok = CHECK_STR(report.md5, expected_md5, "the archive matches its published MD5");
    ok &= CHECK(report.file_count > 0, "'%s' unpacked %lu file(s)",
                tab->code, (unsigned long)report.file_count);
    ok &= CHECK(tab_is_downloaded(tab->code), "'%s' is in the store", tab->code);

    if (verbose)
        printf("      %-4s %6lu bytes, %3lu files, %lu level + %lu challenge\n",
               tab->code, (unsigned long)report.zip_bytes,
               (unsigned long)report.file_count, (unsigned long)report.level_files,
               (unsigned long)report.challenge_files);

    tab_report_free(&report);
    return ok;
}

static void test_single_fetch(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("online_fetch");
    digest *dig;
    const npp_tab *tab;
    config *cfg;
    json_value *entry;
    char *levels, *file;

    test_case("fetching one tab");
    test_use_root(root);

    CHECK(digest_ensure_fresh(1, err, sizeof err) == 0, "digest ready (%s)", err);
    dig = digest_load(err, sizeof err);
    if (!dig) { CHECK(0, "digest load: %s", err); free(root); return; }

    tab = digest_find(dig, SAMPLE_CODE);
    CHECK(tab != NULL, "'%s' is published", SAMPLE_CODE);
    if (tab && fetch_and_verify(dig, tab, 0)) {
        /* The files really are on disk, under the levels folder. */
        char *tab_root = tab_dir_path(tab->code);
        levels = path_join(tab_root, digest_levels_dir(dig));
        file = path_join(levels, "SI.txt");
        CHECK(plat_is_file(file), "the level file it promises is there");
        free(file);
        free(levels);
        free(tab_root);

        cfg = config_load(err, sizeof err);
        entry = cfg ? config_find_tab(cfg, tab->code) : NULL;
        CHECK(entry && json_get_bool(entry, CJK_DOWNLOADED, 0),
              "the download is recorded in the state file");
        config_free(cfg);
    }

    digest_free(dig);
    free(root);
}

/*
 * The whole catalogue: every archive downloaded, its size and MD5 checked
 * against the digest, every entry decompressed and CRC-verified, and every
 * promised level and challenge file confirmed present.
 */
static void test_full_sweep(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("online_full");
    digest *dig;
    size_t i, ok = 0;

    test_case("every published tab");
    test_use_root(root);

    CHECK(digest_ensure_fresh(1, err, sizeof err) == 0, "digest ready (%s)", err);
    dig = digest_load(err, sizeof err);
    if (!dig) { CHECK(0, "digest load: %s", err); free(root); return; }

    for (i = 0; i < dig->tab_count; i++) {
        const npp_tab *tab = &dig->tabs[i];
        tab_remove_report removed;

        if (fetch_and_verify(dig, tab, 1))
            ok++;
        /* Free the disk again as we go: the catalogue is tens of megabytes. */
        tab_remove(tab->code, tab->id, &removed, err, sizeof err);
        tab_remove_report_free(&removed);
    }
    CHECK_NUM(ok, dig->tab_count, "every tab downloaded and verified");

    digest_free(dig);
    free(root);
}

/*
 * The 3rd party server itself. Nothing names one here, so this is the built-in
 * address: the one a fresh install would be pointed at. Any answer is a pass;
 * today the endpoint does not exist yet and 404 is what comes back.
 */
static void test_server_health_live(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("online_server");
    config *cfg;
    server_health health;

    test_case("the live 3rd party server");
    test_use_root(root);

    cfg = config_load(err, sizeof err);
    if (!cfg) { CHECK(0, "config: %s", err); free(root); return; }

    CHECK(server_check(cfg, NULL, &health) == 1, "%s answers (%s)", health.url, health.detail);
    CHECK(health.status > 0, "it replies with a status (HTTP %d)", health.status);
    CHECK_NUM(health.source, SERVER_FROM_DEFAULT, "the built-in host was used");
    printf("      %s -> HTTP %d\n", health.url, health.status);

    config_free(cfg);
    free(root);
}

void suite_online(int full)
{
    test_suite("online");
    test_server_health_live();
    test_digest_download();
    test_single_fetch();
    if (full)
        test_full_sweep();
    else
        printf("  (full sweep skipped, pass --full to include)\n");
}
