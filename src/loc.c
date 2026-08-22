#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "loc.h"
#include "platform.h"
#include "util.h"

/* ---- What a custom tab replaces ---------------------------------------- */

/*
 * The strings a tab changes. Extending this list is all it takes to change one
 * more: everything else — recording the originals, restoring them, reporting
 * what happened — works off the table rather than off the entries in it.
 *
 * The replacements are English whatever column they are written into. Custom
 * texts in eleven languages would be a lot of work for a handful of labels,
 * and an English label in a Spanish menu still says what the panel shows,
 * which the original no longer does.
 */
const loc_replacement loc_replacements[LOC_REPLACEMENT_COUNT] = {
    /* The friend highscore panel is the speedrun boards in a custom tab. */
    { "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG",  LOC_TEXT_LITERAL,
      "Speedrun Boards", "Friends Highscores" },
    { "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT", LOC_TEXT_LITERAL,
      "Speedrun",        "Friends" },
    /* ...and the title screen names the tab that is installed. */
    { "PLAYER_PRESS_ANY",                         LOC_TEXT_TAB_NAME,
      NULL,              "Press Any Key" },
};

/* A copy of `text` with its first letter uppercased. Caller frees. */
static char *capitalized(const char *text)
{
    char *out = str_dup(text ? text : "");

    if (out[0])
        out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

char *loc_replacement_text(const loc_replacement *rep, const npp_tab *tab)
{
    if (rep->kind == LOC_TEXT_TAB_NAME)
        return capitalized(tab ? tab->name : "");
    return str_dup(rep->text ? rep->text : "");
}

const char *loc_outcome_text(loc_outcome outcome)
{
    switch (outcome) {
        case LOC_CHANGED: return "changed";
        case LOC_ALREADY: return "already read that way";
        default:          return "the game has no string of that name";
    }
}

/* ---- Which languages to write ------------------------------------------ */

int loc_langs_parse(const char *text, loc_langs *out)
{
    char *trimmed;
    const char *p;

    memset(out, 0, sizeof(*out));
    if (!text)
        return -1;

    trimmed = str_trim_copy(text, text + strlen(text));
    if (str_ieq(trimmed, LOC_LANGS_ALL_WORD) || str_ieq(trimmed, LOC_LANGS_NONE_WORD)) {
        out->kind = str_ieq(trimmed, LOC_LANGS_ALL_WORD) ? LOC_LANGS_ALL : LOC_LANGS_NONE;
        free(trimmed);
        return 0;
    }
    free(trimmed);

    out->kind = LOC_LANGS_SOME;
    for (p = text; *p; ) {
        const char *end = p;
        char *name;

        while (*end && *end != LOC_LANGS_SEP)
            end++;
        name = str_trim_copy(p, end);
        /* Kept as the user spelled it, so a warning echoes what they wrote. */
        if (name[0] && !str_list_contains(&out->names, name))
            str_list_push(&out->names, name);
        else
            free(name);
        p = *end ? end + 1 : end;
    }

    if (out->names.count == 0) {   /* "", ",", " , " and the like */
        loc_langs_free(out);
        return -1;
    }
    return 0;
}

void loc_langs_free(loc_langs *langs)
{
    str_list_free(&langs->names);
    memset(langs, 0, sizeof(*langs));
}

/* ---- The file ---------------------------------------------------------- */

/* Number of fields a line holds, which is one more than its separators. */
static size_t field_count(const char *line)
{
    size_t count = 1;

    for (; *line; line++) {
        if (*line == LOC_FIELD_SEP)
            count++;
    }
    return count;
}

/* Start of the `index`-th field, or NULL when the line is shorter than that. */
static const char *field_start(const char *line, size_t index)
{
    size_t n = 0;

    for (; n < index; line++) {
        if (!*line)
            return NULL;
        if (*line == LOC_FIELD_SEP)
            n++;
    }
    return line;
}

/* The `index`-th field of a line, or NULL when it has none. Caller frees. */
static char *field_get(const char *line, size_t index)
{
    const char *start = field_start(line, index), *end;

    if (!start)
        return NULL;
    for (end = start; *end && *end != LOC_FIELD_SEP; end++)
        ;
    return str_fmt("%.*s", (int)(end - start), start);
}

/* A copy of `line` with its `index`-th field replaced. Caller frees. */
static char *field_set(const char *line, size_t index, const char *value)
{
    const char *start = field_start(line, index), *end;

    if (!start)
        return str_dup(line);
    for (end = start; *end && *end != LOC_FIELD_SEP; end++)
        ;
    return str_fmt("%.*s%s%s", (int)(start - line), line, value, end);
}

/* Index of the line naming `id`, or -1. Line 0 is the header, never a match. */
static long find_line(const text_lines *file, const char *id)
{
    size_t i;

    for (i = 1; i < file->lines.count; i++) {
        char *found = field_get(file->lines.items[i], 0);
        int hit = found && strcmp(found, id) == 0;

        free(found);
        if (hit)
            return (long)i;
    }
    return -1;
}

/* Past a UTF-8 byte order mark, which an edited file may have picked up. */
static const char *skip_bom(const char *text)
{
    return strncmp(text, "\xEF\xBB\xBF", 3) == 0 ? text + 3 : text;
}

/*
 * The languages the header names, in field order: out->items[i] is field i+1
 * of every line. Fails when the file is not the game's string table, which is
 * worth catching before rewriting nine hundred lines of it.
 */
static int read_languages(const text_lines *file, const char *path, str_list *out,
                          char *err, size_t errsz)
{
    char *first = file->lines.count ? field_get(file->lines.items[0], 0) : NULL;
    size_t i, fields;

    if (!first || strcmp(skip_bom(first), LOC_HEADER_ID) != 0) {
        err_set(err, errsz, "'%s' does not look like the game's text table: its first "
                            "field is not '%s'", path, LOC_HEADER_ID);
        free(first);
        return -1;
    }
    free(first);

    fields = field_count(file->lines.items[0]);
    for (i = 1; i < fields; i++) {
        char *name = field_get(file->lines.items[0], i);

        str_list_push(out, name ? name : str_dup(""));
    }
    if (out->count == 0) {
        err_set(err, errsz, "'%s' names no language at all", path);
        return -1;
    }
    return 0;
}

/*
 * Which fields the caller's choice of languages comes down to. Names the file
 * does not carry go to `unknown` rather than stopping anything: a language the
 * game dropped is no reason to refuse an install. Returns how many fields were
 * resolved, filling `names` in step with them.
 */
static size_t resolve_languages(const loc_langs *langs, const str_list *have,
                                str_list *names, str_list *unknown, size_t **fields)
{
    size_t i, j, count = 0;

    *fields = xmalloc((have->count ? have->count : 1) * sizeof(**fields));
    if (langs->kind == LOC_LANGS_NONE)
        return 0;

    if (langs->kind == LOC_LANGS_ALL) {
        for (i = 0; i < have->count; i++) {
            (*fields)[count++] = i + 1;
            str_list_push(names, str_dup(have->items[i]));
        }
        return count;
    }

    for (i = 0; i < langs->names.count; i++) {
        for (j = 0; j < have->count; j++) {
            if (str_ieq(langs->names.items[i], have->items[j]))
                break;
        }
        if (j == have->count) {
            str_list_push(unknown, str_dup(langs->names.items[i]));
            continue;
        }
        (*fields)[count++] = j + 1;
        str_list_push(names, str_dup(have->items[j]));
    }
    return count;
}

/* ---- Planning ---------------------------------------------------------- */

/* One string on its way to being rewritten, and how it went. */
struct loc_change {
    char *id;
    char *text;         /* what it will read, NULL when each language differs */
    size_t changed;
    size_t already;
    loc_outcome outcome;
};

/* The game's string table. Caller frees. */
static char *table_path(const npp_paths *paths)
{
    char *assets = path_join(paths->install_dir, NPP_ASSETS_SUBDIR);
    char *path = path_join(assets, LOC_FILE_NAME);

    free(assets);
    return path;
}

/*
 * Writes one language of one string, unless it already reads that way. What is
 * overwritten goes into `record` first, when there is one, so the change can be
 * undone without tabber having to know the game's own texts.
 */
static void set_field(text_lines *file, size_t line, size_t field, const char *language,
                      const char *value, loc_change *item, json_value *record)
{
    char *current = field_get(file->lines.items[line], field);
    char *rewritten;

    if (!current)              /* a line shorter than the header: nothing there */
        return;
    if (strcmp(current, value) == 0) {
        item->already++;
        free(current);
        return;
    }

    if (record) {
        json_value *entry = (json_value *)json_get(record, item->id);

        if (!entry) {
            entry = json_new_object();
            json_object_set(record, item->id, entry);
        }
        json_object_set(entry, language, json_new_string(current));
    }

    rewritten = field_set(file->lines.items[line], field, value);
    free(file->lines.items[line]);
    file->lines.items[line] = rewritten;
    item->changed++;
    free(current);
}

/* Serialises the edited file, but only when an edit was actually made. */
static void finish_plan(loc_plan *plan, const text_lines *file)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        if (plan->items[i].changed) {
            plan->updated = text_lines_join(file, &plan->updated_len);
            return;
        }
    }
}

/* Reads the file and its header, the first half of either kind of plan. */
static int open_table(loc_plan *plan, text_lines *file, str_list *have,
                      char *err, size_t errsz)
{
    plan->original = plat_read_file(plan->path, &plan->original_len);
    if (!plan->original) {
        err_set(err, errsz, "cannot read the game's text table at '%s'", plan->path);
        return -1;
    }
    text_lines_split(plan->original, plan->original_len, file);
    return read_languages(file, plan->path, have, err, errsz);
}

int loc_plan_build(const npp_paths *paths, const npp_tab *tab, const loc_langs *langs,
                   loc_plan *plan, char *err, size_t errsz)
{
    text_lines file;
    str_list have = {0};
    size_t *fields = NULL, count, i, k;
    int rc = -1;

    memset(&file, 0, sizeof file);
    memset(plan, 0, sizeof(*plan));
    plan->path = table_path(paths);
    plan->record = json_new_object();

    /* Asked to leave the texts alone, we do not even open the file: a game
     * whose table is missing is then no obstacle to installing a tab. */
    if (langs->kind == LOC_LANGS_NONE) {
        rc = 0;
        goto done;
    }
    if (open_table(plan, &file, &have, err, errsz) != 0)
        goto done;

    count = resolve_languages(langs, &have, &plan->languages, &plan->unknown, &fields);
    if (count == 0) {          /* not one of the languages asked for is there */
        rc = 0;
        goto done;
    }

    plan->items = xmalloc(LOC_REPLACEMENT_COUNT * sizeof(*plan->items));
    for (i = 0; i < LOC_REPLACEMENT_COUNT; i++) {
        const loc_replacement *rep = &loc_replacements[i];
        loc_change *item = &plan->items[plan->count++];
        long line = find_line(&file, rep->id);

        memset(item, 0, sizeof(*item));
        item->id = str_dup(rep->id);
        item->text = loc_replacement_text(rep, tab);
        if (line < 0) {
            /* A string the game no longer has: worth saying, not worth
             * stopping for. The rest still go in. */
            item->outcome = LOC_ABSENT;
            continue;
        }
        for (k = 0; k < count; k++)
            set_field(&file, (size_t)line, fields[k], plan->languages.items[k],
                      item->text, item, plan->record);
        item->outcome = item->changed ? LOC_CHANGED : LOC_ALREADY;
    }
    finish_plan(plan, &file);
    rc = 0;

done:
    free(fields);
    str_list_free(&have);
    text_lines_free(&file);
    if (rc != 0)
        loc_plan_free(plan);
    return rc;
}

/* ---- Restoring --------------------------------------------------------- */

/* The field `language` occupies, or 0 when the file does not carry it. */
static size_t language_field(const str_list *have, const char *language)
{
    size_t i;

    for (i = 0; i < have->count; i++) {
        if (str_ieq(have->items[i], language))
            return i + 1;
    }
    return 0;
}

/* Notes a language as one this plan worked on, once. */
static void note_language(loc_plan *plan, const char *language)
{
    if (!str_list_contains(&plan->languages, language))
        str_list_push(&plan->languages, str_dup(language));
}

/* Puts one string back the way the record says it was. */
static void restore_recorded(loc_plan *plan, text_lines *file, const str_list *have,
                             const json_value *entry, loc_change *item, size_t line)
{
    const json_value *member;

    for (member = entry ? entry->children : NULL; member; member = member->next) {
        size_t field;

        if (member->type != JSON_STRING || !member->key)
            continue;
        field = language_field(have, member->key);
        if (!field) {
            /* The record names a language the file no longer has. */
            if (!str_list_contains(&plan->unknown, member->key))
                str_list_push(&plan->unknown, str_dup(member->key));
            continue;
        }
        note_language(plan, have->items[field - 1]);
        set_field(file, line, field, member->key, member->string, item, NULL);
    }
}

/*
 * The English texts tabber's predecessor overwrote without recording anything.
 * Restoring those needs the originals hardcoded, and is only done when the
 * record does not cover them and the file really has been changed: a string
 * that already reads as the game shipped it is left exactly as it is.
 */
static void restore_legacy(loc_plan *plan, text_lines *file, const str_list *have,
                           const loc_replacement *rep, const json_value *entry,
                           loc_change *item, size_t line)
{
    const json_value *member;
    size_t field;

    if (!rep || !rep->legacy)
        return;
    for (member = entry ? entry->children : NULL; member; member = member->next) {
        if (member->key && str_ieq(member->key, LOC_LEGACY_LANG))
            return;                 /* the record has it: that one wins */
    }
    field = language_field(have, LOC_LEGACY_LANG);
    if (!field)
        return;
    note_language(plan, have->items[field - 1]);
    set_field(file, line, field, LOC_LEGACY_LANG, rep->legacy, item, NULL);
}

/* The replacement carrying this LOC_ID, or NULL for one we do not make. */
static const loc_replacement *replacement_for(const char *id)
{
    size_t i;

    for (i = 0; i < LOC_REPLACEMENT_COUNT; i++) {
        if (strcmp(loc_replacements[i].id, id) == 0)
            return &loc_replacements[i];
    }
    return NULL;
}

int loc_restore_build(const npp_paths *paths, const json_value *record,
                      loc_plan *plan, char *err, size_t errsz)
{
    text_lines file;
    str_list have = {0}, ids = {0};
    const json_value *member;
    size_t i;
    int rc = -1;

    memset(&file, 0, sizeof file);
    memset(plan, 0, sizeof(*plan));
    plan->path = table_path(paths);
    plan->restoring = 1;

    if (open_table(plan, &file, &have, err, errsz) != 0)
        goto done;

    /*
     * Every string we could have changed, whether the record knows about it or
     * not: the ones this version replaces come first, so their English
     * originals can be put back even without a record, and anything else the
     * record carries follows, so a record written by a later version is not
     * quietly ignored.
     */
    for (i = 0; i < LOC_REPLACEMENT_COUNT; i++)
        str_list_push(&ids, str_dup(loc_replacements[i].id));
    for (member = record ? record->children : NULL; member; member = member->next) {
        if (member->key && !str_list_contains(&ids, member->key))
            str_list_push(&ids, str_dup(member->key));
    }

    plan->items = xmalloc(ids.count * sizeof(*plan->items));
    for (i = 0; i < ids.count; i++) {
        const json_value *entry = json_get(record, ids.items[i]);
        loc_change *item = &plan->items[plan->count++];
        long line = find_line(&file, ids.items[i]);

        memset(item, 0, sizeof(*item));
        item->id = str_dup(ids.items[i]);
        if (entry && entry->type != JSON_OBJECT)
            entry = NULL;                 /* a hand-edited record: ignore it */
        if (line < 0) {
            item->outcome = LOC_ABSENT;
            continue;
        }
        restore_recorded(plan, &file, &have, entry, item, (size_t)line);
        restore_legacy(plan, &file, &have, replacement_for(ids.items[i]), entry,
                       item, (size_t)line);
        item->outcome = item->changed ? LOC_CHANGED : LOC_ALREADY;
    }
    finish_plan(plan, &file);
    rc = 0;

done:
    str_list_free(&ids);
    str_list_free(&have);
    text_lines_free(&file);
    if (rc != 0)
        loc_plan_free(plan);
    return rc;
}

/* ---- Applying ---------------------------------------------------------- */

static void fill_report(const loc_plan *plan, loc_report *report)
{
    size_t i;

    memset(report, 0, sizeof(*report));
    report->path = str_dup(plan->path);
    report->restoring = plan->restoring;
    report->count = plan->count;
    report->items = plan->count ? xmalloc(plan->count * sizeof(*report->items)) : NULL;

    for (i = 0; i < plan->count; i++) {
        const loc_change *item = &plan->items[i];
        loc_item *line = &report->items[i];

        line->id = str_dup(item->id);
        line->text = str_dup(item->text);
        line->changed = item->changed;
        line->already = item->already;
        line->outcome = item->outcome;
        report->changed += item->changed;
    }
    for (i = 0; i < plan->languages.count; i++)
        str_list_push(&report->languages, str_dup(plan->languages.items[i]));
    for (i = 0; i < plan->unknown.count; i++)
        str_list_push(&report->unknown, str_dup(plan->unknown.items[i]));
}

int loc_plan_apply(loc_plan *plan, loc_report *report, char *err, size_t errsz)
{
    if (plan->updated && plat_write_file_atomic(plan->path, plan->updated, plan->updated_len) != 0) {
        err_set(err, errsz, "cannot write the game's text table at '%s'", plan->path);
        fill_report(plan, report);
        return -1;
    }
    if (plan->updated)
        plan->applied = 1;
    fill_report(plan, report);
    return 0;
}

void loc_plan_undo(loc_plan *plan)
{
    if (!plan->applied)
        return;
    plat_write_file_atomic(plan->path, plan->original, plan->original_len);
    plan->applied = 0;
}

json_value *loc_plan_take_record(loc_plan *plan)
{
    json_value *record = plan->record;

    plan->record = NULL;
    return record;
}

void loc_plan_free(loc_plan *plan)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        free(plan->items[i].id);
        free(plan->items[i].text);
    }
    free(plan->items);
    free(plan->path);
    free(plan->original);
    free(plan->updated);
    str_list_free(&plan->languages);
    str_list_free(&plan->unknown);
    json_free(plan->record);
    memset(plan, 0, sizeof(*plan));
}

void loc_report_free(loc_report *report)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        free(report->items[i].id);
        free(report->items[i].text);
    }
    free(report->items);
    free(report->path);
    str_list_free(&report->languages);
    str_list_free(&report->unknown);
    memset(report, 0, sizeof(*report));
}
