/*
 * test_state.c - config.json and the choice of 3rd party server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "digest.h"
#include "json.h"
#include "patch.h"
#include "platform.h"
#include "save.h"
#include "server.h"
#include "test.h"
#include "usage.h"
#include "util.h"

/* Loads the state file, asserting it worked. */
static config *load_config(void)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);

    CHECK(cfg != NULL, "config_load succeeded (%s)", cfg ? "" : err);
    return cfg;
}

static void save_config(config *cfg)
{
    char err[TB_ERR_LEN];
    CHECK(config_save(cfg, err, sizeof err) == 0, "config_save succeeded (%s)", err);
}

static void test_new_entries(void)
{
    char *root = test_dir("state_new");
    config *cfg;
    json_value *entry;

    test_case("a fresh state file");
    test_use_root(root);

    cfg = load_config();
    if (!cfg) { free(root); return; }

    /* A tab the tool has never seen starts with everything at its default. */
    entry = config_tab_entry(cfg, 7, "abc");
    CHECK(entry != NULL, "an entry is created on demand");
    CHECK_NUM(json_get_int(entry, CJK_ID, -1), 7, "id");
    CHECK_STR(json_get_string(entry, CJK_CODE, ""), "abc", "code");
    CHECK(!json_get_bool(entry, CJK_DOWNLOADED, 1), "downloaded starts false");
    CHECK(!json_get_bool(entry, CJK_INSTALLED, 1), "installed starts false");
    CHECK(json_get(entry, CJK_DOWNLOAD_DATE)->type == JSON_NULL, "download_date starts null");
    CHECK(json_get(entry, CJK_INSTALL_DATE)->type == JSON_NULL, "install_date starts null");
    CHECK(json_get(entry, CJK_UNINSTALL_DATE)->type == JSON_NULL, "uninstall_date starts null");
    CHECK(json_get(entry, CJK_REMOVE_DATE)->type == JSON_NULL, "remove_date starts null");

    /* Looking it up again must not add a second one. */
    config_tab_entry(cfg, 7, "ABC");
    CHECK_NUM(json_count(json_get(cfg->root, CJK_TABS)), 1, "codes match case-insensitively");

    save_config(cfg);
    config_free(cfg);
    free(root);
}

static void test_lifecycle(void)
{
    char *root = test_dir("state_life");
    config *cfg;
    json_value *entry;
    char install_date[64];

    test_case("download, install, uninstall, remove");
    test_use_root(root);
    cfg = load_config();
    if (!cfg) { free(root); return; }

    config_set_downloaded(cfg, 3, "xyz");
    entry = config_find_tab(cfg, "xyz");
    CHECK(entry && json_get_bool(entry, CJK_DOWNLOADED, 0), "downloaded is set");
    CHECK(json_get(entry, CJK_DOWNLOAD_DATE)->type == JSON_STRING, "download_date is stamped");

    config_set_installed(cfg, 3, "xyz");
    CHECK(json_get_bool(entry, CJK_INSTALLED, 0), "installed is set");
    CHECK(json_get(entry, CJK_INSTALL_DATE)->type == JSON_STRING, "install_date is stamped");
    snprintf(install_date, sizeof install_date, "%s", json_get_string(entry, CJK_INSTALL_DATE, ""));

    config_set_uninstalled(cfg, 3, "xyz");
    CHECK(!json_get_bool(entry, CJK_INSTALLED, 1), "installed is cleared");
    CHECK(json_get(entry, CJK_UNINSTALL_DATE)->type == JSON_STRING, "uninstall_date is stamped");
    /* When it was last installed is worth keeping. */
    CHECK_STR(json_get_string(entry, CJK_INSTALL_DATE, ""), install_date,
              "install_date survives an uninstall");

    CHECK(config_set_removed(cfg, 3, "xyz") == 1, "removal is recorded");
    CHECK(!json_get_bool(entry, CJK_DOWNLOADED, 1), "downloaded is cleared");
    CHECK(json_get(entry, CJK_REMOVE_DATE)->type == JSON_STRING, "remove_date is stamped");
    CHECK(json_get(entry, CJK_DOWNLOAD_DATE)->type == JSON_STRING,
          "download_date survives, as usage history");

    /* Installing implies the files are there, whatever the flag said before. */
    config_set_installed(cfg, 3, "xyz");
    CHECK(json_get_bool(entry, CJK_DOWNLOADED, 0),
          "installing asserts the tab is downloaded");

    /* A tab with no history and no known id is not invented. */
    CHECK(config_set_removed(cfg, -1, "nope") == 0, "removal of an unknown tab records nothing");
    CHECK(config_find_tab(cfg, "nope") == NULL, "...and creates no entry");

    config_free(cfg);
    free(root);
}

static void test_preservation(void)
{
    char *root = test_dir("state_keep");
    char *path;
    config *cfg;
    json_value *entry;
    char *text;

    test_case("hand-edited state survives a rewrite");
    test_use_root(root);
    path = config_path();

    /* A file with keys this version knows nothing about. */
    test_write(path,
        "{\n"
        "  \"theme\": \"dark\",\n"
        "  \"my_setting\": [1, 2, 3],\n"
        "  \"tabs\": [\n"
        "    { \"id\": 5, \"code\": \"keep\", \"downloaded\": true, \"installed\": true,\n"
        "      \"download_date\": \"2020-01-01T00:00:00Z\", \"install_date\": \"2020-02-02T00:00:00Z\",\n"
        "      \"uninstall_date\": null, \"remove_date\": null, \"notes\": \"mine\" }\n"
        "  ]\n"
        "}\n");

    cfg = load_config();
    if (!cfg) { free(path); free(root); return; }
    config_set_downloaded(cfg, 5, "keep");   /* touches only its own fields */
    save_config(cfg);
    config_free(cfg);

    cfg = load_config();
    if (!cfg) { free(path); free(root); return; }
    CHECK_STR(json_get_string(cfg->root, "theme", ""), "dark", "unknown top-level key kept");
    CHECK_NUM(json_count(json_get(cfg->root, "my_setting")), 3, "unknown array kept");
    entry = config_find_tab(cfg, "keep");
    CHECK(entry != NULL, "the entry is still there");
    CHECK(json_get_bool(entry, CJK_INSTALLED, 0), "a field we do not own is untouched");
    CHECK_STR(json_get_string(entry, CJK_INSTALL_DATE, ""), "2020-02-02T00:00:00Z",
              "install_date untouched");
    CHECK_STR(json_get_string(entry, "notes", ""), "mine", "unknown entry field kept");
    CHECK(json_get_bool(entry, CJK_DOWNLOADED, 0), "the field we do own was updated");
    config_free(cfg);

    /* A missing tabs array is added rather than treated as an error. */
    test_write(path, "{\"theme\":\"light\"}");
    cfg = load_config();
    if (cfg) {
        CHECK(json_get(cfg->root, CJK_TABS) != NULL, "a missing tabs array is added");
        config_free(cfg);
    }

    /* Something we cannot parse must be left exactly as it is. */
    test_write(path, "{ \"tabs\": [ this is not json");
    {
        char err[TB_ERR_LEN];
        config *broken = config_load(err, sizeof err);
        CHECK(broken == NULL, "a corrupt state file is refused");
        text = test_read(path);
        CHECK_STR(text, "{ \"tabs\": [ this is not json", "...and left untouched");
        free(text);
        config_free(broken);
    }

    free(path);
    free(root);
}

static void test_state_key(void)
{
    char *root = test_dir("state_flag");
    config *cfg;

    test_case("the state key");
    test_use_root(root);
    cfg = load_config();
    if (!cfg) { free(root); return; }

    config_set_state_library(cfg, 1);
    CHECK(json_get_bool(json_get(cfg->root, CJK_STATE), CJK_LIBRARY, 0), "library true");
    config_set_state_library(cfg, 0);
    CHECK(!json_get_bool(json_get(cfg->root, CJK_STATE), CJK_LIBRARY, 1), "library false");
    CHECK_NUM(json_count(json_get(cfg->root, CJK_STATE)), 1, "the state object holds one key");

    config_free(cfg);
    free(root);
}

/* ---- Server ------------------------------------------------------------ */

static void test_server_parsing(void)
{
    char err[TB_ERR_LEN];
    server_addr addr;
    json_value *root;
    char *uri;

    test_case("reading a server from JSON");
    root = json_parse("{\"server\":{\"scheme\":\"https\",\"host\":\"h.example\",\"port\":443}}",
                      err, sizeof err);
    CHECK(root && server_from_json(root, &addr), "a full server object is read");
    if (root) {
        CHECK_STR(addr.scheme, "https", "scheme");
        CHECK_STR(addr.host, "h.example", "host");
        CHECK_NUM(addr.port, 443, "port");
        json_free(root);
    }

    root = json_parse("{\"server\":{\"host\":\"h2\",\"port\":80}}", err, sizeof err);
    CHECK(root && server_from_json(root, &addr), "the scheme is optional");
    if (root) {
        CHECK_STR(addr.scheme, "", "an absent scheme stays empty");
        json_free(root);
    }

    root = json_parse("{\"server\":{\"scheme\":\"ftp\",\"host\":\"h3\"}}", err, sizeof err);
    CHECK(root && server_from_json(root, &addr), "an unknown scheme does not sink the object");
    if (root) {
        CHECK_STR(addr.scheme, "", "...but is discarded");
        json_free(root);
    }

    root = json_parse("{\"server\":{\"port\":1}}", err, sizeof err);
    CHECK(root && !server_from_json(root, &addr), "a server without a host is unusable");
    json_free(root);

    root = json_parse("{}", err, sizeof err);
    CHECK(root && !server_from_json(root, &addr), "no server key at all");
    json_free(root);

    /* Formatting, with and without the scheme. */
    memset(&addr, 0, sizeof addr);
    snprintf(addr.host, sizeof addr.host, "%s", "outte.ovh");
    addr.port = 8126;
    uri = server_uri(&addr, 1);
    CHECK_STR(uri, "http://outte.ovh:8126", "an absent scheme formats as http");
    free(uri);
    uri = server_uri(&addr, 0);
    CHECK_STR(uri, "outte.ovh:8126", "the scheme can be left out");
    free(uri);
}

static void test_server_precedence(void)
{
    char err[TB_ERR_LEN];
    char *root_dir = test_dir("server_pref");
    config *cfg;
    digest *dig;
    server_addr addr;
    server_source source;
    char *digest_path;
    str_list known = {0};

    test_case("where the server comes from");
    test_use_root(root_dir);

    /* A digest that names a server of its own. */
    digest_path = digest_cache_path();
    {
        char *doctored = str_fmt(
            "{\"config\":{},\"tabs\":[],"
            "\"server\":{\"host\":\"digest.example\",\"port\":1}}");
        test_write(digest_path, doctored);
        free(doctored);
    }
    dig = digest_load(err, sizeof err);
    cfg = config_load(err, sizeof err);
    CHECK(dig && cfg, "fixtures loaded");
    if (!dig || !cfg) goto done;

    /* Nothing in config.json, so the digest wins over the built-in. */
    server_resolve(cfg, dig, 0, &addr, &source);
    CHECK_NUM(source, SERVER_FROM_DIGEST, "the digest outranks the built-in host");
    CHECK_STR(addr.host, "digest.example", "digest host");

    /* config.json outranks everything. */
    json_object_set(cfg->root, SJK_SERVER, json_new_object());
    json_object_set((json_value *)json_get(cfg->root, SJK_SERVER), SJK_HOST,
                    json_new_string("config.example"));
    json_object_set((json_value *)json_get(cfg->root, SJK_SERVER), SJK_PORT,
                    json_new_number(2));
    server_resolve(cfg, dig, 0, &addr, &source);
    CHECK_NUM(source, SERVER_FROM_CONFIG, "config.json outranks the digest");
    CHECK_STR(addr.host, "config.example", "config host");

    /* Every form a patched library might carry, from all four sources. */
    server_known_uris(cfg, dig, &known);
    CHECK(str_list_contains(&known, "config.example:2"), "config form, bare");
    CHECK(str_list_contains(&known, "http://config.example:2"), "config form, with scheme");
    CHECK(str_list_contains(&known, "digest.example:1"), "digest form");
    CHECK(str_list_contains(&known, "outte.ovh:8126"), "built-in host");
    CHECK(str_list_contains(&known, "http://outte.ovh:8126"), "built-in host with scheme");
    CHECK(str_list_contains(&known, "45.32.150.168:8126"), "built-in address");
    CHECK_NUM(known.count, 8, "four sources, two forms each, no duplicates");
    str_list_free(&known);

done:
    config_free(cfg);
    digest_free(dig);
    free(digest_path);
    free(root_dir);
}

/*
 * The patched URI has to fit where the official one was. That is what forces
 * the scheme to be dropped for an address literal.
 */
static void test_uri_budget(void)
{
    char err[TB_ERR_LEN];
    server_addr addr;
    char *uri;

    test_case("building the patched URI");
    CHECK_NUM(LIB_URI_BUDGET, 28, "the official URI is 28 bytes");

    memset(&addr, 0, sizeof addr);
    snprintf(addr.host, sizeof addr.host, "%s", "outte.ovh");
    addr.port = 8126;
    uri = lib_build_uri(&addr, "ctp", LIB_URI_BUDGET, err, sizeof err);
    CHECK_STR(uri, "http://outte.ovh:8126/ctp", "the scheme is kept when it fits");
    free(uri);

    /* http:// would make this 29 bytes, one over. */
    memset(&addr, 0, sizeof addr);
    snprintf(addr.host, sizeof addr.host, "%s", "45.32.150.168");
    addr.port = 8126;
    uri = lib_build_uri(&addr, "ctp", LIB_URI_BUDGET, err, sizeof err);
    CHECK_STR(uri, "45.32.150.168:8126/ctp", "the scheme is dropped when it must be");
    free(uri);

    /* HTTPS cannot lose its scheme without changing what the game speaks. */
    memset(&addr, 0, sizeof addr);
    snprintf(addr.scheme, sizeof addr.scheme, "%s", "https");
    snprintf(addr.host, sizeof addr.host, "%s", "myserver.example.com");
    addr.port = 8126;
    uri = lib_build_uri(&addr, "ctp", LIB_URI_BUDGET, err, sizeof err);
    CHECK(uri == NULL, "an over-long https server is refused");
    CHECK(strstr(err, "scheme cannot be dropped") != NULL, "...for the right reason");
    free(uri);
}

/*
 * The health check. Everything here talks to the loopback address on a port
 * nothing listens on, so it stays inside this machine: a refused connection is
 * exactly the failure the check has to report.
 */
static void test_server_health(void)
{
    char err[TB_ERR_LEN];
    char *root_dir = test_dir("server_health");
    server_addr addr;
    server_health health;
    config *cfg;

    test_case("the server health check");

    memset(&addr, 0, sizeof addr);
    snprintf(addr.host, sizeof addr.host, "%s", TEST_DEAD_HOST);
    addr.port = TEST_DEAD_PORT;

    CHECK(server_probe(&addr, SERVER_FROM_DIGEST, &health) == 0,
          "a port nothing listens on fails the check");
    CHECK_STR(health.url, "http://" TEST_DEAD_HOST ":9/health",
              "the health endpoint is asked for");
    CHECK_NUM(health.reachable, 0, "nothing answered");
    CHECK_NUM(health.status, 0, "so there is no status");
    CHECK(health.detail[0] != '\0', "and the reason is reported");
    CHECK_NUM(health.source, SERVER_FROM_DIGEST, "the source is carried through");
    CHECK_STR(health.addr.host, TEST_DEAD_HOST, "so is the address");

    /* The probe is an HTTP request of ours, so it always carries a scheme. */
    snprintf(addr.scheme, sizeof addr.scheme, "%s", SCHEME_HTTPS);
    server_probe(&addr, SERVER_FROM_CONFIG, &health);
    CHECK_STR(health.url, "https://" TEST_DEAD_HOST ":9/health",
              "an https server is asked over https");

    /* server_check picks the address the same way an install would. */
    test_use_root(root_dir);
    cfg = config_load(err, sizeof err);
    CHECK(cfg != NULL, "state file ready (%s)", cfg ? "" : err);
    if (cfg) {
        json_value *node = json_new_object();
        json_object_set(node, SJK_HOST, json_new_string(TEST_DEAD_HOST));
        json_object_set(node, SJK_PORT, json_new_number(TEST_DEAD_PORT));
        json_object_set(cfg->root, SJK_SERVER, node);

        CHECK(server_check(cfg, NULL, &health) == 0, "the check resolves and fails");
        CHECK_NUM(health.source, SERVER_FROM_CONFIG, "it used the configured server");
        CHECK_STR(health.url, "http://" TEST_DEAD_HOST ":9/health",
                  "and asked that one");
        config_free(cfg);
    }

    free(root_dir);
}

/*
 * When a tab was last played, which nothing records: the answer is read off
 * whichever savefile belongs to the tab, and falls back through the state file
 * to the archive an older installer would have left.
 */
static void test_usage(void)
{
    char *root = test_dir("usage_root");
    char *personal = test_dir("usage_personal");
    char *gz_path, *raw_path, *archive_path;
    long long expected = 0;
    long long now = (long long)time(NULL);
    npp_paths paths;
    tab_usage used;
    config *cfg;

    test_case("when a tab was last played");
    test_use_root(root);
    memset(&paths, 0, sizeof paths);
    paths.personal_dir = personal;   /* borrowed: freed below, not by paths */

    cfg = load_config();
    if (!cfg) { free(root); free(personal); return; }

    gz_path = path_join(personal, SAVE_GZ_NAME);
    raw_path = path_join(personal, SAVE_NAME);
    archive_path = path_join(personal, "nprofile_abc.zip");

    /* Nothing written down and nothing on disk. */
    usage_last_played(cfg, &paths, "abc", 0, &used);
    CHECK_NUM(used.source, USAGE_NEVER, "a tab with no history was never played");
    CHECK_NUM(used.when, 0, "...and carries no date");

    /* A tab one of the older installers handled: no record of it anywhere,
     * but the archive their uninstall wrote is still in the folder. */
    test_write(archive_path, "not really a zip");
    usage_last_played(cfg, &paths, "abc", 0, &used);
    CHECK_NUM(used.source, USAGE_ARCHIVED_SAVE, "its archived save stands in");
    CHECK(used.when >= now, "...dated when that archive was written");

    /* Once tabber has written it down, that is the better answer. */
    config_set_installed(cfg, 1, "abc");
    config_set_uninstalled(cfg, 1, "abc");
    usage_last_played(cfg, &paths, "abc", 0, &used);
    CHECK_NUM(used.source, USAGE_RECORDED, "a recorded uninstall outranks the archive");
    CHECK(used.when >= now, "...and is dated then");

    /* Installed, so the live save is this tab's. Only the uncompressed form
     * is there, which is what an older build of the game writes. */
    test_write(raw_path, "a savefile");
    plat_file_mtime(raw_path, &expected);
    usage_last_played(cfg, &paths, "abc", 1, &used);
    CHECK_NUM(used.source, USAGE_LIVE_SAVE, "an installed tab is dated by the live save");
    CHECK_NUM(used.when, expected, "...the uncompressed one, when that is all there is");

    /* With both there the game reads the gzipped one, and so do we. */
    test_write(gz_path, "a gzipped savefile");
    plat_file_mtime(gz_path, &expected);
    usage_last_played(cfg, &paths, "abc", 1, &used);
    CHECK_NUM(used.source, USAGE_LIVE_SAVE, "still the live save");
    CHECK_NUM(used.when, expected, "...and the gzipped form is the one asked");

    /* Installed but never launched since: no savefile at all, so the install
     * itself is the most recent thing that happened to the tab. */
    plat_remove_file(gz_path);
    plat_remove_file(raw_path);
    usage_last_played(cfg, &paths, "abc", 1, &used);
    CHECK_NUM(used.source, USAGE_RECORDED, "with no savefile, the install date");

    /* And with nothing written down either, it is genuinely unknown. */
    usage_last_played(NULL, &paths, "abc", 1, &used);
    CHECK_NUM(used.source, USAGE_NEVER, "no save and no record is never played");

    /* A folder we know nothing about must not stop the state file answering. */
    usage_last_played(cfg, NULL, "abc", 0, &used);
    CHECK_NUM(used.source, USAGE_RECORDED, "no game folder still reads the record");

    free(gz_path);
    free(raw_path);
    free(archive_path);
    config_free(cfg);
    free(root);
    free(personal);
}

void suite_state(void)
{
    test_suite("state");
    test_new_entries();
    test_lifecycle();
    test_preservation();
    test_state_key();
    test_server_parsing();
    test_server_precedence();
    test_uri_budget();
    test_server_health();
    test_usage();
}
