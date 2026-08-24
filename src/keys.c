#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "keys.h"
#include "platform.h"
#include "tabs.h"
#include "util.h"

/*
 * The three actions that drive a ninja. The game names a setting after the
 * player and the action, so these plus a number are all it takes to find them.
 */
const char *const keys_actions[KEYS_ACTION_COUNT] = { "left", "right", "jump" };

char *keys_setting_name(int player, const char *action)
{
    return str_fmt(KEYS_NAME_FMT, player, action);
}

const char *key_outcome_text(key_outcome outcome)
{
    switch (outcome) {
        case KEY_BOUND:    return "bound";
        case KEY_RESTORED: return "restored";
        case KEY_CLEARED:  return "cleared";
        case KEY_SAME:     return "already read that way";
        default:           return "the file has no such setting";
    }
}

/* ---- The player list --------------------------------------------------- */

/*
 * Appends a player, unless it is already there: naming one twice is harmless
 * and counts once, whether the list came from a command or from the digest.
 */
static void players_add(int *players, size_t *count, int value)
{
    size_t i;

    for (i = 0; i < *count; i++) {
        if (players[i] == value)
            return;
    }
    players[(*count)++] = value;
}

int keys_players_parse(const char *text, int *players, size_t *count,
                       char *err, size_t errsz)
{
    const char *p;

    *count = 0;
    if (!text || !*text) {
        err_set(err, errsz, "no player was named; give a list like '1,2'");
        return -1;
    }

    /*
     * Every comma separates two entries, the last one included: "1," names a
     * player and then nothing, which is a slip worth pointing out rather than
     * quietly reading as "1".
     */
    for (p = text;;) {
        const char *end = p;
        char *token;
        int value = 0, digits = 0;
        size_t i;

        while (*end && *end != KEYS_LIST_SEP)
            end++;
        token = str_trim_copy(p, end);

        for (i = 0; token[i]; i++) {
            if (!isdigit((unsigned char)token[i])) {
                digits = 0;
                break;
            }
            value = value * 10 + (token[i] - '0');
            digits++;
        }
        if (!digits || value < KEYS_PLAYER_MIN || value > KEYS_PLAYER_MAX) {
            if (!token[0])
                err_set(err, errsz, "a player is missing from the list");
            else
                err_set(err, errsz, "'%s' is not a player; use numbers from %d to %d",
                        token, KEYS_PLAYER_MIN, KEYS_PLAYER_MAX);
            free(token);
            return -1;
        }
        free(token);
        players_add(players, count, value);

        if (!*end)
            break;
        p = end + 1;
    }

    if (*count == 0) {
        err_set(err, errsz, "no player was named; give a list like '1,2'");
        return -1;
    }
    return 0;
}

int keys_players_wanted(const npp_tab *tab, int *players, size_t *count,
                        char *err, size_t errsz)
{
    const json_value *list = json_get(json_get(tab->node, TJK_DISK), KJK_BIND);
    const json_value *item;

    *count = 0;
    if (!list || list->type == JSON_NULL)
        return 0;                     /* the usual case: the tab needs none */
    if (list->type != JSON_ARRAY) {
        err_set(err, errsz, "the digest's '%s' for this tab is not a list of players",
                KJK_BIND);
        return -1;
    }

    for (item = list->children; item; item = item->next) {
        double value = item->type == JSON_NUMBER ? item->number : 0;

        if (item->type != JSON_NUMBER || value != (double)(int)value ||
            (int)value < KEYS_PLAYER_MIN || (int)value > KEYS_PLAYER_MAX) {
            err_set(err, errsz, "the digest asks this tab to bind something that is not "
                                "a player; use whole numbers from %d to %d",
                    KEYS_PLAYER_MIN, KEYS_PLAYER_MAX);
            *count = 0;
            return -1;
        }
        players_add(players, count, (int)value);
    }

    /* One player is one player's own keys: nothing to copy, and a digest that
     * says so is more likely wrong than deliberate. */
    if (*count == 1) {
        err_set(err, errsz, "the digest asks this tab to bind player %d to itself",
                players[0]);
        *count = 0;
        return -1;
    }
    return 0;
}

/* The player a setting belongs to, or 0 when its name is not one of theirs. */
static int player_of(const char *name)
{
    int player;

    if (sscanf(name, KEYS_NAME_PREFIX, &player) != 1)
        return 0;
    return player >= KEYS_PLAYER_MIN && player <= KEYS_PLAYER_MAX ? player : 0;
}

/* ---- The file ---------------------------------------------------------- */

/* The bindings file, next to the savefile. Caller frees. */
static char *keys_file_path(const npp_paths *paths)
{
    return path_join(paths->personal_dir, KEYS_FILE_NAME);
}

/*
 * Locates the value of "name = value;" on `line`: the span between the equals
 * sign and the semicolon, trimmed. Everything outside it, the terminator and
 * any comment after it included, is left exactly where it is when the value is
 * rewritten. Returns 0 when the line sets something else.
 */
static int line_value(const char *line, const char *name, size_t *start, size_t *end)
{
    size_t len = strlen(name);
    const char *p = line, *stop;

    while (isspace((unsigned char)*p))
        p++;
    if (strncmp(p, name, len) != 0)
        return 0;
    p += len;
    while (isspace((unsigned char)*p))
        p++;
    if (*p != KEYS_ASSIGN)
        return 0;                    /* a longer name that starts the same way */
    p++;
    while (isspace((unsigned char)*p))
        p++;

    *start = (size_t)(p - line);
    stop = strchr(p, KEYS_TERMINATOR);
    if (!stop)
        stop = p + strlen(p);
    while (stop > p && isspace((unsigned char)stop[-1]))
        stop--;
    *end = (size_t)(stop - line);
    return 1;
}

/* Where `name` is set, or 0 when the file does not set it at all. */
static int find_setting(const text_lines *file, const char *name, size_t *index,
                        size_t *start, size_t *end)
{
    size_t i;

    for (i = 0; i < file->lines.count; i++) {
        if (line_value(file->lines.items[i], name, start, end)) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

/* What `name` is set to, or NULL when it is not set. Caller frees. */
static char *setting_value(const text_lines *file, const char *name)
{
    size_t index, start, end;

    if (!find_setting(file, name, &index, &start, &end))
        return NULL;
    return str_fmt("%.*s", (int)(end - start), file->lines.items[index] + start);
}

/* Writes a value in place, leaving the rest of the line byte for byte. */
static void write_value(text_lines *file, size_t index, size_t start, size_t end,
                        const char *value)
{
    const char *line = file->lines.items[index];

    text_lines_set(file, index,
                   str_fmt("%.*s%s%s", (int)start, line, value, line + end));
}

/* ---- Planning ---------------------------------------------------------- */

/* One binding on its way to being changed. */
struct keys_change {
    char *name;
    char *before, *after;
    int player;
    key_outcome outcome;
};

/* A copy of the state file's record, which a bind adds its originals to. */
static json_value *record_copy(const json_value *record)
{
    json_value *out = json_new_object();
    const json_value *member;

    for (member = record && record->type == JSON_OBJECT ? record->children : NULL;
         member; member = member->next) {
        if (member->type == JSON_STRING && member->key)
            json_object_set(out, member->key, json_new_string(member->string));
    }
    return out;
}

/* Reads the file, the first half of either kind of plan. */
static int open_bindings(keys_plan *plan, text_lines *file, char *err, size_t errsz)
{
    plan->original = plat_read_file(plan->path, &plan->original_len);
    if (!plan->original) {
        err_set(err, errsz, "cannot read the game's key bindings at '%s'", plan->path);
        return -1;
    }
    text_lines_split(plan->original, plan->original_len, file);
    return 0;
}

/*
 * Sets one binding, unless it already reads that way. `record` is where the
 * value being overwritten is kept, and is NULL when nothing is being changed
 * that would need putting back.
 */
static void set_binding(keys_plan *plan, text_lines *file, const char *name,
                        const char *value, key_outcome outcome, json_value *record)
{
    keys_change *item = &plan->items[plan->count++];
    size_t index, start, end;

    memset(item, 0, sizeof(*item));
    item->name = str_dup(name);
    item->player = player_of(name);
    item->after = str_dup(value);

    if (!find_setting(file, name, &index, &start, &end)) {
        item->outcome = KEY_ABSENT;
        return;
    }
    item->before = str_fmt("%.*s", (int)(end - start), file->lines.items[index] + start);
    if (strcmp(item->before, value) == 0) {
        item->outcome = KEY_SAME;
        return;
    }

    /*
     * An original already on record stays: it is what the binding read before
     * we ever touched it, while what is there now is only what an earlier bind
     * left behind.
     */
    if (record && !json_get(record, name))
        json_object_set(record, name, json_new_string(item->before));

    write_value(file, index, start, end, value);
    item->outcome = outcome;
}

/* Serialises the edited file, but only when an edit was actually made. */
static void finish_plan(keys_plan *plan, const text_lines *file)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        key_outcome outcome = plan->items[i].outcome;

        if (outcome != KEY_SAME && outcome != KEY_ABSENT) {
            plan->updated = text_lines_join(file, &plan->updated_len);
            return;
        }
    }
}

int keys_bind_build(const npp_paths *paths, const int *players, size_t count,
                    const json_value *record, keys_plan *plan, char *err, size_t errsz)
{
    text_lines file;
    char *source[KEYS_ACTION_COUNT];
    size_t i, a, have = 0;
    int rc = -1;

    memset(&file, 0, sizeof file);
    memset(plan, 0, sizeof(*plan));
    plan->path = keys_file_path(paths);
    plan->record = record_copy(record);
    plan->source = count ? players[0] : 0;

    if (count < 2) {
        err_set(err, errsz, "binding needs at least two players: the one whose keys "
                            "are copied, and one to copy them to");
        goto done;
    }
    if (open_bindings(plan, &file, err, errsz) != 0)
        goto done;

    /* The keys everyone else is about to answer to. */
    for (a = 0; a < KEYS_ACTION_COUNT; a++) {
        char *name = keys_setting_name(players[0], keys_actions[a]);

        source[have] = setting_value(&file, name);
        if (!source[have]) {
            err_set(err, errsz, "'%s' has no '%s' setting; is this the game's bindings "
                                "file?", plan->path, name);
            free(name);
            goto done;
        }
        have++;
        free(name);
    }

    plan->items = xmalloc((count - 1) * KEYS_ACTION_COUNT * sizeof(*plan->items));
    for (i = 1; i < count; i++) {
        for (a = 0; a < KEYS_ACTION_COUNT; a++) {
            char *name = keys_setting_name(players[i], keys_actions[a]);

            set_binding(plan, &file, name, source[a], KEY_BOUND, plan->record);
            free(name);
        }
    }
    finish_plan(plan, &file);
    rc = 0;

done:
    for (a = 0; a < have; a++)
        free(source[a]);
    text_lines_free(&file);
    if (rc != 0)
        keys_plan_free(plan);
    return rc;
}

/* Whether the record covers a player at all, which one binding of theirs is
 * enough to prove: it can only be there because we put it there. */
static int player_recorded(const json_value *record, int player)
{
    size_t a;

    for (a = 0; a < KEYS_ACTION_COUNT; a++) {
        char *name = keys_setting_name(player, keys_actions[a]);
        int found = json_get(record, name) != NULL;

        free(name);
        if (found)
            return 1;
    }
    return 0;
}

int keys_unbind_build(const npp_paths *paths, const int *players, size_t count,
                      const json_value *record, keys_plan *plan, char *err, size_t errsz)
{
    text_lines file;
    const json_value *member;
    size_t i, a, room;
    int rc = -1;

    memset(&file, 0, sizeof file);
    memset(plan, 0, sizeof(*plan));
    plan->path = keys_file_path(paths);
    plan->record = json_new_object();     /* nothing is changed once this runs */
    plan->restoring = 1;

    if (record && record->type != JSON_OBJECT)
        record = NULL;                    /* a hand-edited state file */
    if (open_bindings(plan, &file, err, errsz) != 0)
        goto done;

    room = json_count(record) + count * KEYS_ACTION_COUNT;
    plan->items = xmalloc((room ? room : 1) * sizeof(*plan->items));

    /* Everything on record goes back to exactly what it was. */
    for (member = record ? record->children : NULL; member; member = member->next) {
        if (member->type == JSON_STRING && member->key)
            set_binding(plan, &file, member->key, member->string, KEY_RESTORED, NULL);
    }

    /*
     * A player named on the command line whose bindings were never recorded
     * gets them cleared instead. That is the fallback for controls an older
     * installer bound: the originals are gone, and -1 at least undoes the
     * sharing rather than leaving two players on one key.
     */
    for (i = 0; i < count; i++) {
        if (player_recorded(record, players[i]))
            continue;
        for (a = 0; a < KEYS_ACTION_COUNT; a++) {
            char *name = keys_setting_name(players[i], keys_actions[a]);

            set_binding(plan, &file, name, KEYS_UNBOUND, KEY_CLEARED, NULL);
            free(name);
        }
    }
    finish_plan(plan, &file);
    rc = 0;

done:
    text_lines_free(&file);
    if (rc != 0)
        keys_plan_free(plan);
    return rc;
}

/* ---- Applying ---------------------------------------------------------- */

static void fill_report(const keys_plan *plan, keys_report *report)
{
    size_t i;

    memset(report, 0, sizeof(*report));
    report->path = str_dup(plan->path);
    report->source = plan->source;
    report->restoring = plan->restoring;
    report->count = plan->count;
    report->items = plan->count ? xmalloc(plan->count * sizeof(*report->items)) : NULL;

    for (i = 0; i < plan->count; i++) {
        const keys_change *item = &plan->items[i];
        key_item *line = &report->items[i];

        line->name = str_dup(item->name);
        line->before = str_dup(item->before);
        line->after = str_dup(item->after);
        line->player = item->player;
        line->outcome = item->outcome;
        if (item->outcome != KEY_SAME && item->outcome != KEY_ABSENT)
            report->changed++;
    }
}

int keys_plan_apply(keys_plan *plan, keys_report *report, char *err, size_t errsz)
{
    if (plan->updated &&
        plat_write_file_atomic(plan->path, plan->updated, plan->updated_len) != 0) {
        err_set(err, errsz, "cannot write the game's key bindings at '%s'", plan->path);
        fill_report(plan, report);
        return -1;
    }
    if (plan->updated)
        plan->applied = 1;
    fill_report(plan, report);
    return 0;
}

void keys_plan_undo(keys_plan *plan)
{
    if (!plan->applied)
        return;
    plat_write_file_atomic(plan->path, plan->original, plan->original_len);
    plan->applied = 0;
}

json_value *keys_plan_take_record(keys_plan *plan)
{
    json_value *record = plan->record;

    plan->record = NULL;
    return record;
}

void keys_plan_free(keys_plan *plan)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        free(plan->items[i].name);
        free(plan->items[i].before);
        free(plan->items[i].after);
    }
    free(plan->items);
    free(plan->path);
    free(plan->original);
    free(plan->updated);
    json_free(plan->record);
    memset(plan, 0, sizeof(*plan));
}

void keys_report_free(keys_report *report)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        free(report->items[i].name);
        free(report->items[i].before);
        free(report->items[i].after);
    }
    free(report->items);
    free(report->path);
    memset(report, 0, sizeof(*report));
}
