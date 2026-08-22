#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "platform.h"
#include "util.h"

char *config_path(void)
{
    char *dir = plat_app_root();
    char *path = path_join(dir ? dir : ".", CONFIG_FILENAME);

    free(dir);
    return path;
}

/* A brand new state file: an object with an empty tab list. */
static json_value *config_new_root(void)
{
    json_value *root = json_new_object();

    json_object_set(root, CJK_TABS, json_new_array());
    return root;
}

config *config_load(char *err, size_t errsz)
{
    char *path = config_path();
    char *text;
    config *cfg;
    json_value *root;

    text = plat_read_file(path, NULL);
    if (!text) {
        /* Nothing saved yet, which is the normal state on a first run. */
        root = config_new_root();
    } else {
        char json_err[TB_ERR_LEN];
        const json_value *tabs;

        root = json_parse(text, json_err, sizeof json_err);
        free(text);
        if (!root || root->type != JSON_OBJECT) {
            err_set(err, errsz, "'%s' is not valid JSON (%s); fix or delete it",
                    path, root ? "not an object" : json_err);
            json_free(root);
            free(path);
            return NULL;
        }

        tabs = json_get(root, CJK_TABS);
        if (!tabs) {
            json_object_set(root, CJK_TABS, json_new_array());
        } else if (tabs->type != JSON_ARRAY) {
            err_set(err, errsz, "'%s' has a '%s' key that is not an array; fix or delete it",
                    path, CJK_TABS);
            json_free(root);
            free(path);
            return NULL;
        }
    }

    cfg = xmalloc(sizeof(*cfg));
    cfg->root = root;
    cfg->path = path;
    return cfg;
}

int config_save(config *cfg, char *err, size_t errsz)
{
    char *text = json_serialize(cfg->root, 1);
    char *tmp_path = str_fmt("%s%s", cfg->path, CONFIG_TMP_SUFFIX);
    size_t len = strlen(text);
    int rc = -1;

    /* Stage then swap, so an interrupted write cannot truncate the state. */
    if (plat_write_file(tmp_path, text, len) != 0) {
        err_set(err, errsz, "cannot write '%s'", tmp_path);
        goto done;
    }
    if (plat_replace_file(tmp_path, cfg->path) != 0) {
        err_set(err, errsz, "cannot update '%s'", cfg->path);
        plat_remove_file(tmp_path);
        goto done;
    }
    rc = 0;

done:
    free(text);
    free(tmp_path);
    return rc;
}

void config_free(config *cfg)
{
    if (!cfg)
        return;
    json_free(cfg->root);
    free(cfg->path);
    free(cfg);
}

json_value *config_find_tab(config *cfg, const char *code)
{
    json_value *tabs = (json_value *)json_get(cfg->root, CJK_TABS);
    json_value *entry;

    if (!tabs)
        return NULL;
    for (entry = tabs->children; entry; entry = entry->next) {
        if (entry->type == JSON_OBJECT && str_ieq(json_get_string(entry, CJK_CODE, ""), code))
            return entry;
    }
    return NULL;
}

void config_set_state_library(config *cfg, int ok)
{
    json_value *state = (json_value *)json_get(cfg->root, CJK_STATE);

    if (!state || state->type != JSON_OBJECT) {
        state = json_new_object();
        json_object_set(cfg->root, CJK_STATE, state);
    }
    json_object_set(state, CJK_LIBRARY, json_new_bool(ok));
}

json_value *config_tab_entry(config *cfg, int id, const char *code)
{
    json_value *tabs = (json_value *)json_get(cfg->root, CJK_TABS);
    json_value *entry = config_find_tab(cfg, code);

    /* An existing entry wins, so its history is preserved. */
    if (entry || !tabs)
        return entry;

    entry = json_new_object();
    json_object_set(entry, CJK_ID, json_new_number(id));
    json_object_set(entry, CJK_CODE, json_new_string(code));
    json_object_set(entry, CJK_DOWNLOADED, json_new_bool(0));
    json_object_set(entry, CJK_INSTALLED, json_new_bool(0));
    json_object_set(entry, CJK_DOWNLOAD_DATE, json_new_null());
    json_object_set(entry, CJK_INSTALL_DATE, json_new_null());
    json_object_set(entry, CJK_UNINSTALL_DATE, json_new_null());
    json_object_set(entry, CJK_REMOVE_DATE, json_new_null());
    json_array_append(tabs, entry);
    return entry;
}

void config_set_downloaded(config *cfg, int id, const char *code)
{
    json_value *entry = config_tab_entry(cfg, id, code);
    char now[TB_TIMESTAMP_LEN + 1];

    if (!entry)
        return;
    time_now_iso8601(now, sizeof now);

    /* Keep the id in step with the digest in case the catalogue renumbered. */
    json_object_set(entry, CJK_ID, json_new_number(id));
    json_object_set(entry, CJK_DOWNLOADED, json_new_bool(1));
    json_object_set(entry, CJK_DOWNLOAD_DATE, json_new_string(now));
}

void config_set_installed(config *cfg, int id, const char *code)
{
    json_value *entry = config_tab_entry(cfg, id, code);
    char now[TB_TIMESTAMP_LEN + 1];

    if (!entry)
        return;
    time_now_iso8601(now, sizeof now);

    json_object_set(entry, CJK_ID, json_new_number(id));
    json_object_set(entry, CJK_INSTALLED, json_new_bool(1));
    json_object_set(entry, CJK_INSTALL_DATE, json_new_string(now));

    /*
     * A tab cannot be installed unless its files are in the store, so the flag
     * must be true here. This corrects entries for tabs downloaded before the
     * state file existed; the date stays null, since we do not know it.
     */
    json_object_set(entry, CJK_DOWNLOADED, json_new_bool(1));
}

void config_set_uninstalled(config *cfg, int id, const char *code)
{
    json_value *entry = config_tab_entry(cfg, id, code);
    char now[TB_TIMESTAMP_LEN + 1];

    if (!entry)
        return;
    time_now_iso8601(now, sizeof now);

    json_object_set(entry, CJK_ID, json_new_number(id));
    json_object_set(entry, CJK_INSTALLED, json_new_bool(0));
    json_object_set(entry, CJK_UNINSTALL_DATE, json_new_string(now));
    /* CJK_INSTALL_DATE stays: it records when the tab was last installed. */
}

int config_set_removed(config *cfg, int id, const char *code)
{
    json_value *entry = config_find_tab(cfg, code);
    char now[TB_TIMESTAMP_LEN + 1];

    if (!entry) {
        /* Nothing on record: only start one if we know which tab this is. */
        if (id < 0)
            return 0;
        entry = config_tab_entry(cfg, id, code);
        if (!entry)
            return 0;
    }
    time_now_iso8601(now, sizeof now);

    if (id >= 0)
        json_object_set(entry, CJK_ID, json_new_number(id));
    json_object_set(entry, CJK_DOWNLOADED, json_new_bool(0));
    json_object_set(entry, CJK_REMOVE_DATE, json_new_string(now));
    return 1;
}

void config_set_palettes(config *cfg, int id, const char *code, const str_list *names)
{
    json_value *entry = config_tab_entry(cfg, id, code);
    json_value *array;
    size_t i;

    if (!entry)
        return;
    array = json_new_array();
    for (i = 0; names && i < names->count; i++)
        json_array_append(array, json_new_string(names->items[i]));
    json_object_set(entry, CJK_PALETTES, array);
}

void config_set_strings(config *cfg, json_value *record)
{
    json_object_set(cfg->root, CJK_STRINGS, record ? record : json_new_object());
}

const json_value *config_get_strings(config *cfg)
{
    const json_value *record = json_get(cfg->root, CJK_STRINGS);

    return record && record->type == JSON_OBJECT ? record : NULL;
}

void config_set_keybindings(config *cfg, json_value *record)
{
    json_object_set(cfg->root, CJK_KEYBINDINGS, record ? record : json_new_object());
}

const json_value *config_get_keybindings(config *cfg)
{
    const json_value *record = json_get(cfg->root, CJK_KEYBINDINGS);

    return record && record->type == JSON_OBJECT ? record : NULL;
}

int config_get_palettes(config *cfg, const char *code, str_list *out)
{
    const json_value *array = json_get(config_find_tab(cfg, code), CJK_PALETTES);
    const json_value *item;

    if (!array || array->type != JSON_ARRAY)
        return 0;
    for (item = array->children; item; item = item->next) {
        if (item->type == JSON_STRING && item->string[0])
            str_list_push(out, str_dup(item->string));
    }
    return 1;
}
