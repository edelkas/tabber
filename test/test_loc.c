/*
 * test_loc.c - Replacing the game's own texts, and putting them back.
 *
 * Everything here works on a stand-in loc.txt in a scratch directory, so the
 * real one is never opened. The strongest check the suite has is that a table
 * which has been written to and then restored is the same file byte for byte,
 * and most tests end with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "digest.h"
#include "json.h"
#include "loc.h"
#include "paths.h"
#include "platform.h"
#include "test.h"
#include "util.h"

/*
 * A stand-in for the game's table: three languages, the three strings a tab
 * changes, and a few it does not. The French episode is spelled with the
 * accent the real file uses, so the tests run over multi-byte text.
 */
#define LOC_HEADER  "LOC_ID|english|spanish|french\n"
#define LOC_EPISODE "EPISODE|Episode|Episodio|\xC3\x89" "pisode\n"
#define LOC_LONG    "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG|" \
                    "Friends Highscores|Records de amigos|Records amis\n"
#define LOC_SHORT   "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT|Friends|Amigos|Amis\n"
#define LOC_PRESS   "PLAYER_PRESS_ANY|Press Any Key|Pulsa una tecla|Appuyer sur une touche\n"
#define LOC_LEVEL   "LEVEL|Level|Nivel|Niveau\n"
#define LOC_TABLE   LOC_HEADER LOC_EPISODE LOC_LONG LOC_SHORT LOC_PRESS LOC_LEVEL

/* The columns of that table. */
#define COL_ENGLISH 1
#define COL_SPANISH 2
#define COL_FRENCH  3
#define LOC_LANG_COUNT 3

/* The strings, by the names the game knows them by. */
#define ID_EPISODE  "EPISODE"
#define ID_LONG     "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_LONG"
#define ID_SHORT    "HIGH_SCORE_PANEL_FRIEND_HIGHSCORES_SHORT"
#define ID_PRESS    "PLAYER_PRESS_ANY"
#define ID_LEVEL    "LEVEL"

/* What a tab replaces them with. The tab's name is deliberately lowercase. */
#define TAB_NAME    "nova"
#define WANT_LONG   "Speedrun Boards"
#define WANT_SHORT  "Speedrun"
#define WANT_PRESS  "Nova"

/* ...and what the whole table reads once all three languages are written. */
#define LOC_TABLE_DONE  LOC_HEADER LOC_EPISODE                          \
    ID_LONG  "|" WANT_LONG  "|" WANT_LONG  "|" WANT_LONG  "\n"          \
    ID_SHORT "|" WANT_SHORT "|" WANT_SHORT "|" WANT_SHORT "\n"          \
    ID_PRESS "|" WANT_PRESS "|" WANT_PRESS "|" WANT_PRESS "\n"          \
    LOC_LEVEL

/* ---- Scratch table ----------------------------------------------------- */

typedef struct {
    char *root;       /* the scratch directory          */
    char *install;    /* the stand-in installation      */
    char *path;       /* its loc.txt                    */
    npp_paths paths;  /* borrows `install`, never freed */
} table;

static void table_build(table *t, const char *name, const char *text)
{
    char *assets;

    memset(t, 0, sizeof(*t));
    t->root = test_dir(name);
    t->install = path_join(t->root, "N++");
    assets = path_join(t->install, NPP_ASSETS_SUBDIR);
    t->path = path_join(assets, LOC_FILE_NAME);
    free(assets);

    if (text)
        test_write(t->path, text);
    t->paths.install_dir = t->install;
}

static void table_free(table *t)
{
    free(t->root);
    free(t->install);
    free(t->path);
}

/* One field of one line of a table, by the line's LOC_ID. Caller frees. */
static char *field_of(const char *text, const char *id, size_t column)
{
    size_t id_len = strlen(id);
    const char *line;

    for (line = text; line && *line; ) {
        const char *end = strchr(line, '\n');
        const char *stop = end ? end : line + strlen(line);

        if (!strncmp(line, id, id_len) && line[id_len] == LOC_FIELD_SEP) {
            const char *start = line;
            size_t n = 0;

            while (n < column && start < stop) {
                if (*start++ == LOC_FIELD_SEP)
                    n++;
            }
            if (n < column)
                return NULL;
            for (end = start; end < stop && *end != LOC_FIELD_SEP; end++)
                ;
            if (end > start && end[-1] == '\r')      /* not part of the value */
                end--;
            return str_fmt("%.*s", (int)(end - start), start);
        }
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

#define CHECK_FIELD(text, id, column, want, what) do {                  \
        char *got_ = field_of((text), (id), (column));                  \
        CHECK_STR(got_ ? got_ : "(no such line)", (want), (what));      \
        free(got_);                                                     \
    } while (0)

/* A copy of `text` with every line ending turned into a CRLF. Caller frees. */
static char *crlf_copy(const char *text)
{
    byte_buf out = {0};

    for (; *text; text++) {
        if (*text == '\n')
            buf_append(&out, "\r", 1);
        buf_append(&out, text, 1);
    }
    return buf_finish(&out, NULL);
}

/* Runs a plan the way an install does, reporting what went wrong if it did. */
static int build_and_apply(table *t, const npp_tab *tab, const loc_langs *langs,
                           loc_plan *plan, loc_report *report)
{
    char err[TB_ERR_LEN];

    if (!CHECK(loc_plan_build(&t->paths, tab, langs, plan, err, sizeof err) == 0,
               "the replacements are worked out (%s)", err))
        return -1;
    if (!CHECK(loc_plan_apply(plan, report, err, sizeof err) == 0,
               "and written (%s)", err))
        return -1;
    return 0;
}

/* ---- Options ----------------------------------------------------------- */

static void test_languages_parsed(void)
{
    loc_langs langs;

    test_case("--languages accepts all, none and a list");

    CHECK(loc_langs_parse("all", &langs) == 0 && langs.kind == LOC_LANGS_ALL,
          "'all' means every language");
    loc_langs_free(&langs);

    CHECK(loc_langs_parse("  All  ", &langs) == 0 && langs.kind == LOC_LANGS_ALL,
          "whitespace and case do not matter");
    loc_langs_free(&langs);

    CHECK(loc_langs_parse("none", &langs) == 0 && langs.kind == LOC_LANGS_NONE,
          "'none' leaves the texts alone");
    loc_langs_free(&langs);

    CHECK(loc_langs_parse(" spanish , english ", &langs) == 0 &&
          langs.kind == LOC_LANGS_SOME, "a list is a list");
    CHECK_NUM(langs.names.count, 2, "two languages");
    CHECK_STR(langs.names.items[0], "spanish", "whitespace around a name is stripped");
    CHECK_STR(langs.names.items[1], "english", "and the order is kept");
    loc_langs_free(&langs);

    CHECK(loc_langs_parse("Spanish,spanish,SPANISH", &langs) == 0 &&
          langs.names.count == 1, "the same language twice is still one language");
    loc_langs_free(&langs);

    CHECK(loc_langs_parse("", &langs) != 0, "an empty value is a usage error");
    CHECK(loc_langs_parse(" , ,", &langs) != 0, "and so is a list of nothing");
    CHECK(loc_langs_parse(NULL, &langs) != 0, "...and a missing one");
}

static void test_replacements_listed(void)
{
    npp_tab tab;
    char *text;
    size_t i;

    test_case("the replacement table says what to write");

    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    for (i = 0; i < LOC_REPLACEMENT_COUNT; i++) {
        CHECK(loc_replacements[i].id && loc_replacements[i].id[0],
              "replacement %u names a string", (unsigned)i);
        CHECK(loc_replacements[i].legacy != NULL,
              "...and the original the old installer overwrote");
    }

    text = loc_replacement_text(&loc_replacements[0], &tab);
    CHECK_STR(text, WANT_LONG, "a literal replacement is used as it stands");
    free(text);

    text = loc_replacement_text(&loc_replacements[2], &tab);
    CHECK_STR(text, WANT_PRESS, "the tab's name goes in capitalised");
    free(text);
}

/* ---- Replacing --------------------------------------------------------- */

static void test_replace_every_language(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    json_value *record;
    const json_value *entry;
    char *text;

    test_case("every language of every string is replaced");
    table_build(&t, "loc_all", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        CHECK_NUM(report.count, LOC_REPLACEMENT_COUNT, "one line per replacement");
        CHECK_NUM(report.languages.count, LOC_LANG_COUNT, "all three languages");
        CHECK_NUM(report.unknown.count, 0, "none of them unknown");
        CHECK_NUM(report.changed, LOC_REPLACEMENT_COUNT * LOC_LANG_COUNT,
                  "three strings in three languages");

        text = test_read(t.path);
        CHECK_FIELD(text, ID_LONG, COL_ENGLISH, WANT_LONG, "the panel is renamed");
        CHECK_FIELD(text, ID_LONG, COL_SPANISH, WANT_LONG, "in Spanish too");
        CHECK_FIELD(text, ID_LONG, COL_FRENCH, WANT_LONG, "and in French");
        CHECK_FIELD(text, ID_SHORT, COL_ENGLISH, WANT_SHORT, "the short label as well");
        CHECK_FIELD(text, ID_PRESS, COL_SPANISH, WANT_PRESS,
                    "and the title screen names the tab");
        CHECK_FIELD(text, ID_EPISODE, COL_FRENCH, "\xC3\x89" "pisode",
                    "a string the tab does not touch is left alone, accent and all");
        CHECK_FIELD(text, ID_LEVEL, COL_ENGLISH, "Level", "...and so is the last line");
        CHECK_STR(text, LOC_TABLE_DONE, "the whole table reads as it should");
        free(text);

        record = loc_plan_take_record(&plan);
        entry = json_get(record, ID_SHORT);
        CHECK_STR(json_get_string(entry, "english", ""), "Friends",
                  "the original is recorded, so it can be put back");
        CHECK_STR(json_get_string(entry, "spanish", ""), "Amigos",
                  "one per language changed");
        CHECK_NUM(json_count(record), LOC_REPLACEMENT_COUNT,
                  "and one entry per string");
        json_free(record);

        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_replace_one_language(void)
{
    table t;
    npp_tab tab;
    loc_langs langs;
    loc_plan plan;
    loc_report report;
    json_value *record;
    char *text;

    test_case("naming a language changes that one and no other");
    table_build(&t, "loc_one", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;
    loc_langs_parse("SPANISH", &langs);

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        CHECK_NUM(report.languages.count, 1, "one language");
        CHECK_STR(report.languages.items[0], "spanish",
                  "named as the game spells it, not as the user typed it");
        CHECK_NUM(report.changed, LOC_REPLACEMENT_COUNT, "one field per string");

        text = test_read(t.path);
        CHECK_FIELD(text, ID_LONG, COL_SPANISH, WANT_LONG, "Spanish is replaced");
        CHECK_FIELD(text, ID_LONG, COL_ENGLISH, "Friends Highscores",
                    "English is left as the game shipped it");
        CHECK_FIELD(text, ID_LONG, COL_FRENCH, "Records amis", "and so is French");
        free(text);

        record = loc_plan_take_record(&plan);
        CHECK(json_get(json_get(record, ID_LONG), "english") == NULL,
              "only the language that changed is recorded");
        CHECK_STR(json_get_string(json_get(record, ID_LONG), "spanish", ""),
                  "Records de amigos", "and it is recorded correctly");
        json_free(record);

        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    loc_langs_free(&langs);
    table_free(&t);
}

static void test_unknown_language(void)
{
    table t;
    npp_tab tab;
    loc_langs langs;
    loc_plan plan;
    loc_report report;
    char *text;

    test_case("a language the game does not have is skipped, not fatal");
    table_build(&t, "loc_unknown", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;
    loc_langs_parse("spanish,klingon", &langs);

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        CHECK_NUM(report.unknown.count, 1, "one language was not found");
        CHECK_STR(report.unknown.items[0], "klingon", "reported as the user wrote it");
        CHECK_NUM(report.languages.count, 1, "the other one still goes in");

        text = test_read(t.path);
        CHECK_FIELD(text, ID_SHORT, COL_SPANISH, WANT_SHORT, "Spanish is replaced");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    loc_langs_free(&langs);
    table_free(&t);

    test_case("...and when none of them is found, nothing is written");
    table_build(&t, "loc_unknown_all", LOC_TABLE);
    loc_langs_parse("klingon", &langs);

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        CHECK_NUM(report.count, 0, "there is nothing to report");
        CHECK_NUM(report.unknown.count, 1, "beyond the language that is not there");
        text = test_read(t.path);
        CHECK_STR(text, LOC_TABLE, "the table is untouched, byte for byte");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    loc_langs_free(&langs);
    table_free(&t);
}

static void test_no_language(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_NONE, {0}};
    loc_plan plan;
    loc_report report;
    json_value *record;
    char *text;

    test_case("--languages none leaves the texts exactly as they are");
    table_build(&t, "loc_none", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        CHECK_NUM(report.count, 0, "no string is considered");
        CHECK_NUM(report.changed, 0, "and none is written");
        text = test_read(t.path);
        CHECK_STR(text, LOC_TABLE, "the table is untouched, byte for byte");
        free(text);

        record = loc_plan_take_record(&plan);
        CHECK_NUM(json_count(record), 0, "and there is nothing to remember");
        json_free(record);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_already_replaced(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan first, second;
    loc_report report;
    json_value *record;

    test_case("installing over texts that already read that way changes nothing");
    table_build(&t, "loc_again", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &first, &report) == 0)
        loc_report_free(&report);
    loc_plan_free(&first);

    if (build_and_apply(&t, &tab, &langs, &second, &report) == 0) {
        size_t i, already = 0;

        for (i = 0; i < report.count; i++)
            already += report.items[i].outcome == LOC_ALREADY ? 1 : 0;
        CHECK_NUM(already, LOC_REPLACEMENT_COUNT, "every string already reads so");
        CHECK_NUM(report.changed, 0, "so nothing is written");

        record = loc_plan_take_record(&second);
        CHECK_NUM(json_count(record), 0,
                  "and the replacement is never recorded as an original");
        json_free(record);
        loc_report_free(&report);
    }
    loc_plan_free(&second);
    table_free(&t);
}

static void test_missing_string(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    size_t i, absent = 0;

    test_case("a string the game no longer has is reported, not fatal");
    /* The same table without the line the title screen shows. */
    table_build(&t, "loc_absent", LOC_HEADER LOC_EPISODE LOC_LONG LOC_SHORT LOC_LEVEL);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        char *text = test_read(t.path);

        for (i = 0; i < report.count; i++)
            absent += report.items[i].outcome == LOC_ABSENT ? 1 : 0;
        CHECK_NUM(absent, 1, "one string is missing");
        CHECK_FIELD(text, ID_LONG, COL_ENGLISH, WANT_LONG, "the others still go in");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

/* ---- Restoring --------------------------------------------------------- */

static void test_round_trip(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    json_value *record = NULL;
    char *text;

    test_case("what an install wrote, an uninstall puts back");
    table_build(&t, "loc_round", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        record = loc_plan_take_record(&plan);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);

    {
        char err[TB_ERR_LEN];

        CHECK(loc_restore_build(&t.paths, record, &plan, err, sizeof err) == 0,
              "the originals are worked out (%s)", err);
        CHECK(loc_plan_apply(&plan, &report, err, sizeof err) == 0,
              "and written back (%s)", err);
        CHECK_NUM(report.changed, LOC_REPLACEMENT_COUNT * LOC_LANG_COUNT,
                  "every field that was written is written back");
        CHECK(report.restoring, "and the report says which direction this was");

        text = test_read(t.path);
        CHECK_STR(text, LOC_TABLE, "the table is the file it started as, byte for byte");
        free(text);
        loc_report_free(&report);
        loc_plan_free(&plan);
    }
    json_free(record);
    table_free(&t);
}

static void test_legacy_restore(void)
{
    table t;
    loc_plan plan;
    loc_report report;
    char err[TB_ERR_LEN];
    char *text;

    test_case("texts the old installer changed are restored without a record");
    /* English as the previous installer left it, the other languages untouched. */
    table_build(&t, "loc_legacy",
                LOC_HEADER
                LOC_EPISODE
                ID_LONG  "|" WANT_LONG  "|Records de amigos|Records amis\n"
                ID_SHORT "|" WANT_SHORT "|Amigos|Amis\n"
                ID_PRESS "|Metanet|Pulsa una tecla|Appuyer sur une touche\n"
                LOC_LEVEL);

    CHECK(loc_restore_build(&t.paths, NULL, &plan, err, sizeof err) == 0,
          "a missing record is no obstacle (%s)", err);
    CHECK(loc_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and the originals go back in (%s)", err);
    CHECK_NUM(report.changed, LOC_REPLACEMENT_COUNT, "one field per string, English");
    CHECK_NUM(report.languages.count, 1, "and English only");

    text = test_read(t.path);
    CHECK_STR(text, LOC_TABLE, "the table is the game's own again, byte for byte");
    free(text);

    loc_report_free(&report);
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_legacy_leaves_untouched_texts(void)
{
    table t;
    loc_plan plan;
    loc_report report;
    char err[TB_ERR_LEN];
    char *text;

    test_case("...and a table nobody changed is not 'restored' over");
    table_build(&t, "loc_legacy_clean", LOC_TABLE);

    CHECK(loc_restore_build(&t.paths, NULL, &plan, err, sizeof err) == 0,
          "the plan is built (%s)", err);
    CHECK(loc_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and applies (%s)", err);
    CHECK_NUM(report.changed, 0, "there is nothing to put back");

    text = test_read(t.path);
    CHECK_STR(text, LOC_TABLE, "so the file is not rewritten at all");
    free(text);

    loc_report_free(&report);
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_record_wins_over_legacy(void)
{
    table t;
    loc_plan plan;
    loc_report report;
    json_value *record = json_new_object();
    json_value *entry = json_new_object();
    char err[TB_ERR_LEN];
    char *text;

    test_case("a recorded original beats the hardcoded one, and covers new strings");
    table_build(&t, "loc_record",
                LOC_HEADER
                LOC_EPISODE
                LOC_LONG
                LOC_SHORT
                ID_PRESS "|" WANT_PRESS "|Pulsa una tecla|Appuyer sur une touche\n"
                ID_LEVEL "|Stage|Nivel|Niveau\n");

    /* What the user actually had before the install, English included. */
    json_object_set(entry, "english", json_new_string("Hit It"));
    json_object_set(record, ID_PRESS, entry);
    /* ...and a string this version does not replace, but a later one might. */
    entry = json_new_object();
    json_object_set(entry, "english", json_new_string("Level"));
    json_object_set(record, ID_LEVEL, entry);

    CHECK(loc_restore_build(&t.paths, record, &plan, err, sizeof err) == 0,
          "the plan is built (%s)", err);
    CHECK(loc_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and applies (%s)", err);
    CHECK_NUM(report.changed, 2, "two fields go back");

    text = test_read(t.path);
    CHECK_FIELD(text, ID_PRESS, COL_ENGLISH, "Hit It",
                "the record wins over the original tabber carries");
    CHECK_FIELD(text, ID_LEVEL, COL_ENGLISH, "Level",
                "and a string only the record knows about is restored too");
    free(text);

    loc_report_free(&report);
    loc_plan_free(&plan);
    json_free(record);
    table_free(&t);
}

static void test_record_unknown_language(void)
{
    table t;
    loc_plan plan;
    loc_report report;
    json_value *record = json_new_object();
    json_value *entry = json_new_object();
    char err[TB_ERR_LEN];

    test_case("a record naming a language the game dropped is reported, not fatal");
    table_build(&t, "loc_record_lang", LOC_TABLE);

    json_object_set(entry, "klingon", json_new_string("Friends"));
    json_object_set(record, ID_SHORT, entry);

    CHECK(loc_restore_build(&t.paths, record, &plan, err, sizeof err) == 0,
          "the plan is built (%s)", err);
    CHECK(loc_plan_apply(&plan, &report, err, sizeof err) == 0,
          "and applies (%s)", err);
    CHECK_NUM(report.unknown.count, 1, "the language is reported as unknown");
    CHECK_NUM(report.changed, 0, "and nothing is written");

    loc_report_free(&report);
    loc_plan_free(&plan);
    json_free(record);
    table_free(&t);
}

/* ---- The file itself --------------------------------------------------- */

static void test_line_endings_kept(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    char *crlf = crlf_copy(LOC_TABLE);
    char *text;

    test_case("a table with Windows line endings keeps them");
    table_build(&t, "loc_crlf", crlf);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        char *want = crlf_copy(LOC_TABLE_DONE);

        text = test_read(t.path);
        CHECK_FIELD(text, ID_LONG, COL_FRENCH, WANT_LONG,
                    "the last column is written without eating the line ending");
        CHECK_STR(text, want, "and every line ending survives the rewrite");
        free(want);
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
    free(crlf);
}

static void test_no_trailing_newline(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    /* The same table with its last line unterminated. */
    const char *body = LOC_HEADER LOC_EPISODE LOC_LONG LOC_SHORT LOC_PRESS
                       "LEVEL|Level|Nivel|Niveau";
    char *text;

    test_case("a table that does not end in a newline does not gain one");
    table_build(&t, "loc_no_eol", body);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        text = test_read(t.path);
        CHECK(text && text[strlen(text) - 1] != '\n', "the file still ends where it did");
        CHECK_FIELD(text, ID_LEVEL, COL_FRENCH, "Niveau", "and the last field is intact");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_byte_order_mark(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    char *text;

    test_case("a table saved with a byte order mark still reads as one");
    table_build(&t, "loc_bom", "\xEF\xBB\xBF" LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        text = test_read(t.path);
        CHECK(strncmp(text, "\xEF\xBB\xBF", 3) == 0, "the mark is left where it was");
        CHECK_FIELD(text, ID_SHORT, COL_ENGLISH, WANT_SHORT, "and the strings go in");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

static void test_refuses_other_files(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    char err[TB_ERR_LEN];
    char *text;

    test_case("something that is not the game's table is refused");
    table_build(&t, "loc_wrong", "hello|there\nthis is not loc.txt\n");
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    err[0] = '\0';
    CHECK(loc_plan_build(&t.paths, &tab, &langs, &plan, err, sizeof err) != 0,
          "the plan is refused");
    CHECK(err[0] != '\0', "with a reason: %s", err);
    text = test_read(t.path);
    CHECK_STR(text, "hello|there\nthis is not loc.txt\n", "and the file is untouched");
    free(text);
    loc_plan_free(&plan);
    table_free(&t);

    test_case("...and so is a game with no table at all");
    table_build(&t, "loc_gone", NULL);
    err[0] = '\0';
    CHECK(loc_plan_build(&t.paths, &tab, &langs, &plan, err, sizeof err) != 0,
          "the plan is refused");
    CHECK(err[0] != '\0', "with a reason: %s", err);
    loc_plan_free(&plan);

    /* Unless there was nothing to do with it in the first place, in which case
     * a table we were never going to open is no reason to refuse an install. */
    {
        loc_langs none = {LOC_LANGS_NONE, {0}};

        CHECK(loc_plan_build(&t.paths, &tab, &none, &plan, err, sizeof err) == 0,
              "but --languages none does not need the table at all");
        loc_plan_free(&plan);
    }
    table_free(&t);
}

static void test_undo(void)
{
    table t;
    npp_tab tab;
    loc_langs langs = {LOC_LANGS_ALL, {0}};
    loc_plan plan;
    loc_report report;
    char *text;

    test_case("an install that fails later leaves the texts as it found them");
    table_build(&t, "loc_undo", LOC_TABLE);
    memset(&tab, 0, sizeof tab);
    tab.name = TAB_NAME;

    if (build_and_apply(&t, &tab, &langs, &plan, &report) == 0) {
        text = test_read(t.path);
        CHECK(strstr(text, WANT_LONG) != NULL, "the texts went in");
        free(text);

        loc_plan_undo(&plan);
        text = test_read(t.path);
        CHECK_STR(text, LOC_TABLE, "and came straight back out, byte for byte");
        free(text);
        loc_report_free(&report);
    }
    loc_plan_free(&plan);
    table_free(&t);
}

/* ---- The state record -------------------------------------------------- */

static void test_state_record(void)
{
    char *root = test_dir("loc_state");
    config *cfg;
    json_value *record = json_new_object();
    json_value *entry = json_new_object();
    const json_value *read_back;
    char err[TB_ERR_LEN];

    test_case("the originals survive a trip through the state file");
    test_use_root(root);

    json_object_set(entry, "english", json_new_string("Friends"));
    json_object_set(record, ID_SHORT, entry);

    cfg = config_load(err, sizeof err);
    CHECK(cfg != NULL, "the state file loads (%s)", cfg ? "" : err);
    if (cfg) {
        config_set_strings(cfg, record);
        CHECK(config_save(cfg, err, sizeof err) == 0, "and saves (%s)", err);
        config_free(cfg);

        cfg = config_load(err, sizeof err);
        read_back = cfg ? config_get_strings(cfg) : NULL;
        CHECK(read_back != NULL, "the record is read back");
        CHECK_STR(json_get_string(json_get(read_back, ID_SHORT), "english", ""),
                  "Friends", "with the original in it");

        /* Uninstalling empties it, so it always says what is live right now. */
        config_set_strings(cfg, NULL);
        read_back = config_get_strings(cfg);
        CHECK(read_back != NULL && json_count(read_back) == 0,
              "and an uninstall leaves it empty rather than absent");
        config_free(cfg);
    }
    free(root);
}

/* ---- Suite ------------------------------------------------------------- */

void suite_loc(void)
{
    test_suite("loc");
    test_languages_parsed();
    test_replacements_listed();
    test_replace_every_language();
    test_replace_one_language();
    test_unknown_language();
    test_no_language();
    test_already_replaced();
    test_missing_string();
    test_round_trip();
    test_legacy_restore();
    test_legacy_leaves_untouched_texts();
    test_record_wins_over_legacy();
    test_record_unknown_language();
    test_line_endings_kept();
    test_no_trailing_newline();
    test_byte_order_mark();
    test_refuses_other_files();
    test_undo();
    test_state_record();
}
