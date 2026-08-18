#include <stdlib.h>
#include <string.h>

#include "digest.h"
#include "json.h"
#include "net.h"
#include "platform.h"
#include "util.h"
#include "version.h"

/* Set once the digest has been downloaded in this session. */
static int g_refreshed_this_session = 0;

char *digest_cache_path(void)
{
    char *dir = plat_app_root();
    char *path;

    /* Falling back to the working directory keeps the tool usable even if the
     * executable's location cannot be determined. */
    path = path_join(dir ? dir : ".", DIGEST_FILENAME);
    free(dir);
    return path;
}

/* Rejects a payload that parses but is not a digest, so the cache stays sane. */
static int digest_looks_valid(const char *text, char *err, size_t errsz)
{
    char json_err[TB_ERR_LEN];
    json_value *root = json_parse(text, json_err, sizeof json_err);
    const json_value *tabs;
    int ok;

    if (!root) {
        err_set(err, errsz, "the download is not valid JSON (%s)", json_err);
        return 0;
    }
    tabs = json_get(root, DJK_TABS);
    ok = tabs && tabs->type == JSON_ARRAY;
    if (!ok)
        err_set(err, errsz, "the download has no '%s' array", DJK_TABS);
    json_free(root);
    return ok;
}

int digest_ensure_fresh(int force, char *err, size_t errsz)
{
    char *url_data = NULL, *path = NULL, *tmp_path = NULL;
    size_t len = 0;
    int rc = -1;

    if (!force && g_refreshed_this_session)
        return 0;

    if (net_fetch(DIGEST_URL, &url_data, &len, err, errsz) != 0)
        return -1;

    if (!digest_looks_valid(url_data, err, errsz))
        goto done;

    /* Write to a temporary file and swap it in, so an interrupted run cannot
     * leave a truncated digest behind. */
    path = digest_cache_path();
    tmp_path = str_fmt("%s%s", path, DIGEST_TMP_SUFFIX);
    if (plat_write_file(tmp_path, url_data, len) != 0) {
        err_set(err, errsz, "cannot write '%s'", tmp_path);
        goto done;
    }
    if (plat_replace_file(tmp_path, path) != 0) {
        err_set(err, errsz, "cannot update '%s'", path);
        plat_remove_file(tmp_path);
        goto done;
    }

    g_refreshed_this_session = 1;
    rc = 0;

done:
    free(url_data);
    free(path);
    free(tmp_path);
    return rc;
}

/* Fills one npp_tab from a "tabs" array entry. */
static void digest_read_tab(const json_value *entry, npp_tab *tab, size_t index)
{
    const json_value *attrs = json_get(entry, DJK_ATTRIBUTES);

    tab->node = entry;
    tab->id = (int)json_get_int(attrs, DJK_ID, (long)index);
    tab->code = json_get_string(attrs, DJK_CODE, "");
    tab->name = json_get_string(attrs, DJK_NAME, "");
    tab->authors = json_get_string(attrs, DJK_AUTHORS, "");
    tab->date = json_get_string(attrs, DJK_DATE, "");
    tab->version = (int)json_get_int(attrs, DJK_VERSION, 0);
    tab->enabled = json_get_bool(attrs, DJK_ENABLED, 1);
}

digest *digest_load(char *err, size_t errsz)
{
    char *path = digest_cache_path();
    char *text;
    char json_err[TB_ERR_LEN];
    json_value *root;
    const json_value *tabs, *signature, *entry;
    digest *dig;
    size_t i;

    text = plat_read_file(path, NULL);
    if (!text) {
        err_set(err, errsz, "no digest available at '%s'; run '%s update' while online",
                path, TABBER_NAME);
        free(path);
        return NULL;
    }

    root = json_parse(text, json_err, sizeof json_err);
    free(text);
    if (!root) {
        err_set(err, errsz, "'%s' is corrupt (%s); run '%s update' to refetch it",
                path, json_err, TABBER_NAME);
        free(path);
        return NULL;
    }

    tabs = json_get(root, DJK_TABS);
    if (!tabs || tabs->type != JSON_ARRAY) {
        err_set(err, errsz, "'%s' has no '%s' array", path, DJK_TABS);
        json_free(root);
        free(path);
        return NULL;
    }

    dig = xmalloc(sizeof(*dig));
    memset(dig, 0, sizeof(*dig));
    dig->root = root;
    dig->path = path;
    dig->tab_count = json_count(tabs);
    dig->tabs = dig->tab_count ? xmalloc(dig->tab_count * sizeof(*dig->tabs)) : NULL;

    i = 0;
    for (entry = tabs->children; entry; entry = entry->next) {
        digest_read_tab(entry, &dig->tabs[i], i);
        i++;
    }

    signature = json_get(root, DJK_SIGNATURE);
    dig->signature_date = json_get_string(signature, DJK_DATE, NULL);
    dig->signature_md5 = json_get_string(signature, DJK_MD5, NULL);

    return dig;
}

void digest_free(digest *dig)
{
    if (!dig)
        return;
    json_free(dig->root);
    free(dig->tabs);
    free(dig->path);
    free(dig);
}

const json_value *digest_config(const digest *dig)
{
    return dig ? json_get(dig->root, DJK_CONFIG) : NULL;
}

const char *digest_levels_dir(const digest *dig)
{
    return json_get_string(digest_config(dig), DJK_LEVELS_DIR, DIGEST_DEFAULT_LEVELS_DIR);
}

const char *digest_palettes_dir(const digest *dig)
{
    return json_get_string(digest_config(dig), DJK_PALETTES_DIR, DIGEST_DEFAULT_PALETTES_DIR);
}

const npp_tab *digest_find(const digest *dig, const char *code)
{
    size_t i;

    if (!dig || !code)
        return NULL;
    for (i = 0; i < dig->tab_count; i++) {
        if (str_ieq(dig->tabs[i].code, code))
            return &dig->tabs[i];
    }
    return NULL;
}
