/*
 * test_keys.c - Binding several players' controls together, and undoing it.
 *
 * The bindings file is small enough to write out in full, so most tests here
 * compare the whole of it against what it should read: a rewrite that gets the
 * three settings right but disturbs a comment, a blank line or somebody else's
 * key still fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "keys.h"
#include "paths.h"
#include "platform.h"
#include "test.h"
#include "util.h"

/* The keys each player answers to in the stand-in file. */
#define P1_LEFT   "KEYBIND(\"Left\")"
#define P1_RIGHT  "KEYBIND(\"Right\")"
#define P1_JUMP   "KEYBIND(\"Z\")"
#define P2_LEFT   "KEYBIND(\"A\")"
#define P2_RIGHT  "KEYBIND(\"D\")"
#define P2_JUMP   KEYS_UNBOUND        /* player 2 cannot jump at all yet */
#define P3_LEFT   "KEYBIND(\"F\")"
#define P3_RIGHT  "KEYBIND(\"H\")"
#define P3_JUMP   "KEYBIND(\"F3\")"

/*
 * A stand-in keys.vars: the comments, the blank lines between blocks and the
 * settings that work the menus are all there, because none of them is ours to
 * touch. The "extra" line is a decoy whose name starts with a real one, and
 * comes first, so a match that does not insist on the equals sign finds it.
 */
#define K_HEAD \
    "//Created automatically. Modify at your own risk\n"
#define K_P1(left, right, jump) \
    "//Player 1\n" \
    "input_p1_up_key = KEYBIND(\"Up\");\n" \
    "input_p1_left_key = " left ";\n" \
    "input_p1_right_key = " right ";\n" \
    "input_p1_jump_key = " jump ";\n" \
    "input_p1_back_key = KEYBIND(\"X\");\n" \
    "\n"
#define K_P2(left, right, jump) \
    "//Player 2\n" \
    "input_p2_up_key = KEYBIND(\"W\");\n" \
    "input_p2_left_key_extra = KEYBIND(\"Q\");\n" \
    "input_p2_left_key = " left ";\n" \
    "input_p2_right_key = " right ";\n" \
    "input_p2_jump_key = " jump ";\n" \
    "input_p2_back_key = KEYBIND(\"R\");\n" \
    "\n"
#define K_P3(left, right, jump) \
    "//Player 3\n" \
    "input_p3_up_key = KEYBIND(\"T\");\n" \
    "input_p3_left_key = " left ";\n" \
    "input_p3_right_key = " right ";\n" \
    "input_p3_jump_key = " jump ";\n" \
    "\n"
#define K_P4 \
    "//Player 4\n" \
    "input_p4_up_key = KEYBIND(\"I\");\n" \
    "input_p4_left_key = KEYBIND(\"J\");\n" \
    "input_p4_right_key = KEYBIND(\"L\");\n" \
    "input_p4_jump_key = KEYBIND(\"F4\");\n" \
    "\n"
#define K_MENU \
    "//Menu Keys (Works on Player 1 only)\n" \
    "input_menu_up = KEYBIND(\"Up\");\n" \
    "input_menu_left = KEYBIND(\"Left\");\n"

#define KEYS_TABLE \
    K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) K_P2(P2_LEFT, P2_RIGHT, P2_JUMP) \
    K_P3(P3_LEFT, P3_RIGHT, P3_JUMP) K_P4 K_MENU

/* ...and what it reads once one player has been bound to another. */
#define KEYS_P2_BOUND \
    K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) K_P2(P1_LEFT, P1_RIGHT, P1_JUMP) \
    K_P3(P3_LEFT, P3_RIGHT, P3_JUMP) K_P4 K_MENU
#define KEYS_P2_P3_BOUND \
    K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) K_P2(P1_LEFT, P1_RIGHT, P1_JUMP) \
    K_P3(P1_LEFT, P1_RIGHT, P1_JUMP) K_P4 K_MENU
#define KEYS_P2_CLEARED \
    K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) \
    K_P2(KEYS_UNBOUND, KEYS_UNBOUND, KEYS_UNBOUND) \
    K_P3(P3_LEFT, P3_RIGHT, P3_JUMP) K_P4 K_MENU

/* Settings by name, for the record checks. */
#define P2_LEFT_KEY  "input_p2_left_key"
#define P2_RIGHT_KEY "input_p2_right_key"
#define P2_JUMP_KEY  "input_p2_jump_key"
#define P3_JUMP_KEY  "input_p3_jump_key"

/* ---- Scratch bindings file --------------------------------------------- */

typedef struct {
    char *root;        /* the scratch directory, also the tool's root  */
    char *personal;    /* the stand-in personal folder                 */
    char *path;        /* its keys.vars                                */
    npp_paths paths;   /* borrows `personal`, never freed as a whole   */
} world;

static void world_build(world *w, const char *name, const char *text)
{
    memset(w, 0, sizeof(*w));
    w->root = test_dir(name);
    w->personal = path_join(w->root, "personal");
    w->path = path_join(w->personal, KEYS_FILE_NAME);
    if (text)
        test_write(w->path, text);
    w->paths.personal_dir = w->personal;
    test_use_root(w->root);           /* config.json lands in the scratch too */
}

static void world_free(world *w)
{
    free(w->root);
    free(w->personal);
    free(w->path);
}

/* The bindings file as it now reads. Caller frees. */
static char *world_read(world *w)
{
    return test_read(w->path);
}

/* Binds, applies, and hands back the record the state file would keep. */
static json_value *bind_players(world *w, const int *players, size_t count,
                                const json_value *record, keys_report *report)
{
    char err[TB_ERR_LEN];
    keys_plan plan;
    json_value *out = NULL;

    if (!CHECK(keys_bind_build(&w->paths, players, count, record, &plan,
                               err, sizeof err) == 0,
               "the binding is worked out (%s)", err))
        return NULL;
    if (CHECK(keys_plan_apply(&plan, report, err, sizeof err) == 0,
              "and written (%s)", err))
        out = keys_plan_take_record(&plan);
    keys_plan_free(&plan);
    return out;
}

/* The same for the other direction. */
static json_value *unbind_players(world *w, const int *players, size_t count,
                                  const json_value *record, keys_report *report)
{
    char err[TB_ERR_LEN];
    keys_plan plan;
    json_value *out = NULL;

    if (!CHECK(keys_unbind_build(&w->paths, players, count, record, &plan,
                                 err, sizeof err) == 0,
               "the restore is worked out (%s)", err))
        return NULL;
    if (CHECK(keys_plan_apply(&plan, report, err, sizeof err) == 0,
              "and written (%s)", err))
        out = keys_plan_take_record(&plan);
    keys_plan_free(&plan);
    return out;
}

/* A record of one setting, as an earlier bind would have left it. */
static json_value *record_of(const char *name, const char *value)
{
    json_value *record = json_new_object();

    json_object_set(record, name, json_new_string(value));
    return record;
}

/* ---- The player list --------------------------------------------------- */

static void test_players_parsed(void)
{
    int players[KEYS_PLAYER_MAX];
    size_t count = 0;
    char err[TB_ERR_LEN];

    test_case("a player list is read the way it is written");

    CHECK(keys_players_parse("1,2", players, &count, err, sizeof err) == 0,
          "'1,2' is a list (%s)", err);
    CHECK_NUM(count, 2, "of two players");
    CHECK_NUM(players[0], 1, "the first names the keys to copy");
    CHECK_NUM(players[1], 2, "the second the player to copy them to");

    CHECK(keys_players_parse(" 3 , 1 ", players, &count, err, sizeof err) == 0,
          "whitespace is stripped (%s)", err);
    CHECK_NUM(players[0], 3, "and the order is the caller's, not sorted");

    CHECK(keys_players_parse("2,2,3,2", players, &count, err, sizeof err) == 0,
          "a player named twice is accepted (%s)", err);
    CHECK_NUM(count, 2, "and counted once");

    CHECK(keys_players_parse("1,2,3,4", players, &count, err, sizeof err) == 0,
          "all four players fit (%s)", err);
    CHECK_NUM(count, KEYS_PLAYER_MAX, "with none dropped");

    CHECK(keys_players_parse("0", players, &count, err, sizeof err) != 0,
          "there is no player 0");
    CHECK(keys_players_parse("5", players, &count, err, sizeof err) != 0,
          "...nor a fifth player");
    CHECK(keys_players_parse("one", players, &count, err, sizeof err) != 0,
          "...and a name is not a number");
    CHECK(keys_players_parse("1,", players, &count, err, sizeof err) != 0,
          "a trailing comma names an empty player");
    CHECK(keys_players_parse("", players, &count, err, sizeof err) != 0,
          "and an empty list names nobody");
}

static void test_setting_names(void)
{
    char *name;

    test_case("the settings are named after the player and the action");

    CHECK_NUM(KEYS_ACTION_COUNT, 3, "three actions drive a ninja");
    name = keys_setting_name(2, keys_actions[0]);
    CHECK_STR(name, P2_LEFT_KEY, "left is the first of them");
    free(name);
    name = keys_setting_name(3, "jump");
    CHECK_STR(name, P3_JUMP_KEY, "and jump is one of them");
    free(name);
}

/* ---- Binding ----------------------------------------------------------- */

static void test_bind_one_player(void)
{
    world w;
    keys_report report;
    json_value *record;
    int players[2];
    char *text;

    test_case("one player takes over another's keys, and nothing else moves");
    world_build(&w, "keys_bind", KEYS_TABLE);
    players[0] = 1;
    players[1] = 2;

    record = bind_players(&w, players, 2, NULL, &report);
    if (record) {
        CHECK_NUM(report.count, KEYS_ACTION_COUNT, "three bindings are considered");
        CHECK_NUM(report.changed, KEYS_ACTION_COUNT, "and all three change");
        CHECK_NUM(report.source, 1, "the report names the player they came from");

        text = world_read(&w);
        CHECK_STR(text, KEYS_P2_BOUND, "the file reads as it should, byte for byte");
        free(text);

        CHECK_STR(json_get_string(record, P2_LEFT_KEY, ""), P2_LEFT,
                  "the original is recorded");
        CHECK_STR(json_get_string(record, P2_JUMP_KEY, ""), KEYS_UNBOUND,
                  "even when it was no key at all");
        CHECK_NUM(json_count(record), KEYS_ACTION_COUNT, "one entry per binding");
        CHECK(json_get(record, "input_p2_up_key") == NULL,
              "and nothing is recorded for a setting we do not touch");

        json_free(record);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_bind_two_players(void)
{
    world w;
    keys_report report;
    json_value *record;
    int players[3];
    char *text;

    test_case("...and so do two of them at once");
    world_build(&w, "keys_bind3", KEYS_TABLE);
    players[0] = 1;
    players[1] = 2;
    players[2] = 3;

    record = bind_players(&w, players, 3, NULL, &report);
    if (record) {
        CHECK_NUM(report.count, 2 * KEYS_ACTION_COUNT, "six bindings are considered");
        text = world_read(&w);
        CHECK_STR(text, KEYS_P2_P3_BOUND, "both players answer to player 1's keys");
        free(text);
        CHECK_NUM(json_count(record), 2 * KEYS_ACTION_COUNT, "and six originals are kept");
        json_free(record);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_bind_what_already_matches(void)
{
    world w;
    keys_report report;
    json_value *record;
    int players[2];
    size_t i, same = 0;

    test_case("a binding that already reads that way is not recorded");
    /* Player 3 already shares player 1's keys, so there is nothing to undo. */
    world_build(&w, "keys_bind_same",
                K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) K_P2(P2_LEFT, P2_RIGHT, P2_JUMP)
                K_P3(P1_LEFT, P1_RIGHT, P1_JUMP) K_P4 K_MENU);
    players[0] = 1;
    players[1] = 3;

    record = bind_players(&w, players, 2, NULL, &report);
    if (record) {
        for (i = 0; i < report.count; i++)
            same += report.items[i].outcome == KEY_SAME ? 1 : 0;
        CHECK_NUM(same, KEYS_ACTION_COUNT, "every binding already read that way");
        CHECK_NUM(report.changed, 0, "so none of them is written");
        CHECK_NUM(json_count(record), 0,
                  "and the replacement is never recorded as an original");
        json_free(record);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_bind_twice_keeps_the_original(void)
{
    world w;
    keys_report report;
    json_value *record, *second;
    int players[2];

    test_case("binding again does not record what the first bind left behind");
    world_build(&w, "keys_bind_twice", KEYS_TABLE);
    players[0] = 1;
    players[1] = 2;

    record = bind_players(&w, players, 2, NULL, &report);
    keys_report_free(&report);

    /* Now bind the same player to somebody else, without undoing the first. */
    players[0] = 3;
    second = bind_players(&w, players, 2, record, &report);
    if (second) {
        char *text = world_read(&w);

        CHECK(strstr(text, P2_LEFT_KEY " = " P3_LEFT ";") != NULL,
              "the second bind takes effect");
        free(text);
        CHECK_STR(json_get_string(second, P2_LEFT_KEY, ""), P2_LEFT,
                  "but the record still holds what the player had to begin with");
        CHECK_NUM(json_count(second), KEYS_ACTION_COUNT,
                  "and gains no second entry for the same setting");
        json_free(second);
        keys_report_free(&report);
    }
    json_free(record);
    world_free(&w);
}

static void test_bind_needs_two_players(void)
{
    world w;
    keys_plan plan;
    char err[TB_ERR_LEN];
    int players[1];
    char *text;

    test_case("binding a player to itself is refused");
    world_build(&w, "keys_bind_one", KEYS_TABLE);
    players[0] = 1;

    err[0] = '\0';
    CHECK(keys_bind_build(&w.paths, players, 1, NULL, &plan, err, sizeof err) != 0,
          "one player is not a binding");
    CHECK(err[0] != '\0', "and the reason says so: %s", err);
    keys_plan_free(&plan);

    text = world_read(&w);
    CHECK_STR(text, KEYS_TABLE, "the file is untouched");
    free(text);
    world_free(&w);
}

/* ---- Unbinding --------------------------------------------------------- */

static void test_unbind_restores(void)
{
    world w;
    keys_report report;
    json_value *record, *cleared;
    int players[2];
    char *text;

    test_case("what a bind changed, an unbind puts back");
    world_build(&w, "keys_round", KEYS_TABLE);
    players[0] = 1;
    players[1] = 2;

    record = bind_players(&w, players, 2, NULL, &report);
    keys_report_free(&report);
    if (!record) { world_free(&w); return; }

    cleared = unbind_players(&w, NULL, 0, record, &report);
    if (cleared) {
        CHECK_NUM(report.changed, KEYS_ACTION_COUNT, "three bindings go back");
        CHECK(report.restoring, "and the report says which direction this was");

        text = world_read(&w);
        CHECK_STR(text, KEYS_TABLE, "the file is the one it started as, byte for byte");
        free(text);

        CHECK_NUM(json_count(cleared), 0, "and nothing is on record any more");
        json_free(cleared);
        keys_report_free(&report);
    }
    json_free(record);
    world_free(&w);
}

static void test_unbind_clears_unrecorded(void)
{
    world w;
    keys_report report;
    json_value *cleared;
    int players[1];
    char *text;

    test_case("a player nothing was recorded for is cleared instead");
    /* The file as an older installer would have left it: player 2 on player
     * 1's keys, and no record of what player 2 used to have. */
    world_build(&w, "keys_clear", KEYS_P2_BOUND);
    players[0] = 2;

    cleared = unbind_players(&w, players, 1, NULL, &report);
    if (cleared) {
        CHECK_NUM(report.changed, KEYS_ACTION_COUNT, "all three are written");
        CHECK_NUM(report.items[0].outcome, KEY_CLEARED, "and cleared rather than restored");

        text = world_read(&w);
        CHECK_STR(text, KEYS_P2_CLEARED,
                  "the player is left with no keys, and nobody else is touched");
        free(text);
        json_free(cleared);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_unbind_prefers_the_record(void)
{
    world w;
    keys_report report;
    json_value *record, *cleared;
    int players[1];
    char *text;
    size_t i, restored = 0;

    test_case("one recorded binding is enough to spare a player from clearing");
    world_build(&w, "keys_partial", KEYS_P2_BOUND);
    /* Only the jump was ever recorded, but that proves the player is ours. */
    record = record_of(P2_JUMP_KEY, P2_JUMP);
    players[0] = 2;

    cleared = unbind_players(&w, players, 1, record, &report);
    if (cleared) {
        for (i = 0; i < report.count; i++)
            restored += report.items[i].outcome == KEY_RESTORED ? 1 : 0;
        CHECK_NUM(report.count, 1, "only the recorded binding is considered");
        CHECK_NUM(restored, 1, "and it is restored");

        text = world_read(&w);
        CHECK(strstr(text, P2_JUMP_KEY " = " KEYS_UNBOUND ";") != NULL,
              "the jump is back to what it was");
        CHECK(strstr(text, P2_LEFT_KEY " = " P1_LEFT ";") != NULL,
              "and the two that were never recorded are left as they are, "
              "not cleared");
        free(text);
        json_free(cleared);
        keys_report_free(&report);
    }
    json_free(record);
    world_free(&w);
}

static void test_unbind_mixes_both(void)
{
    world w;
    keys_report report;
    json_value *record, *cleared;
    int players[2];
    char *text;

    test_case("one player restored and another cleared, in the same run");
    world_build(&w, "keys_mixed", KEYS_P2_P3_BOUND);
    record = record_of(P2_LEFT_KEY, P2_LEFT);
    json_object_set(record, P2_RIGHT_KEY, json_new_string(P2_RIGHT));
    json_object_set(record, P2_JUMP_KEY, json_new_string(P2_JUMP));
    players[0] = 2;
    players[1] = 3;

    cleared = unbind_players(&w, players, 2, record, &report);
    if (cleared) {
        text = world_read(&w);
        CHECK_STR(text,
                  K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP) K_P2(P2_LEFT, P2_RIGHT, P2_JUMP)
                  K_P3(KEYS_UNBOUND, KEYS_UNBOUND, KEYS_UNBOUND) K_P4 K_MENU,
                  "the recorded player is restored and the other one cleared");
        free(text);
        json_free(cleared);
        keys_report_free(&report);
    }
    json_free(record);
    world_free(&w);
}

static void test_unbind_with_nothing_to_do(void)
{
    world w;
    char err[TB_ERR_LEN];
    keys_plan plan;
    char *text;

    test_case("no record and no player named leaves everything alone");
    world_build(&w, "keys_nothing", KEYS_TABLE);

    CHECK(keys_unbind_build(&w.paths, NULL, 0, NULL, &plan, err, sizeof err) == 0,
          "the plan is built (%s)", err);
    CHECK_NUM(plan.count, 0, "with nothing in it");
    keys_plan_free(&plan);

    text = world_read(&w);
    CHECK_STR(text, KEYS_TABLE, "and the file is untouched");
    free(text);
    world_free(&w);
}

/* ---- The file itself --------------------------------------------------- */

static void test_layout_is_kept(void)
{
    world w;
    keys_report report;
    json_value *record;
    int players[2];
    char *text;

    test_case("odd spacing, tabs and Windows line endings all survive");
    world_build(&w, "keys_layout",
                "//Player 1\r\n"
                "input_p1_left_key\t=\tKEYBIND(\"Left\") ;\r\n"
                "input_p1_right_key = KEYBIND(\"Right\");\r\n"
                "input_p1_jump_key = KEYBIND(\"Z\");\r\n"
                "\r\n"
                "input_p2_left_key   =   KEYBIND(\"A\");   //was Q\r\n"
                "input_p2_right_key = KEYBIND(\"D\");\r\n"
                "input_p2_jump_key = -1;\r\n");
    players[0] = 1;
    players[1] = 2;

    record = bind_players(&w, players, 2, NULL, &report);
    if (record) {
        text = world_read(&w);
        CHECK_STR(text,
                  "//Player 1\r\n"
                  "input_p1_left_key\t=\tKEYBIND(\"Left\") ;\r\n"
                  "input_p1_right_key = KEYBIND(\"Right\");\r\n"
                  "input_p1_jump_key = KEYBIND(\"Z\");\r\n"
                  "\r\n"
                  "input_p2_left_key   =   KEYBIND(\"Left\");   //was Q\r\n"
                  "input_p2_right_key = KEYBIND(\"Right\");\r\n"
                  "input_p2_jump_key = KEYBIND(\"Z\");\r\n",
                  "only the values change: the spacing, the comment and the "
                  "line endings are all where they were");
        free(text);
        CHECK_STR(json_get_string(record, P2_LEFT_KEY, ""), P2_LEFT,
                  "and the value read back is the value alone");
        json_free(record);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_missing_settings(void)
{
    world w;
    keys_plan plan;
    keys_report report;
    json_value *record;
    char err[TB_ERR_LEN];
    int players[2];
    size_t i, absent = 0;

    test_case("a file that does not set the source's keys is refused");
    world_build(&w, "keys_no_source", K_HEAD K_P2(P2_LEFT, P2_RIGHT, P2_JUMP));
    players[0] = 1;
    players[1] = 2;

    err[0] = '\0';
    CHECK(keys_bind_build(&w.paths, players, 2, NULL, &plan, err, sizeof err) != 0,
          "there is nothing to copy");
    CHECK(err[0] != '\0', "and the reason says so: %s", err);
    keys_plan_free(&plan);
    world_free(&w);

    test_case("...but a target that is missing one is only reported");
    world_build(&w, "keys_no_target",
                K_HEAD K_P1(P1_LEFT, P1_RIGHT, P1_JUMP)
                "input_p2_left_key = " P2_LEFT ";\n"
                "input_p2_right_key = " P2_RIGHT ";\n");

    record = bind_players(&w, players, 2, NULL, &report);
    if (record) {
        char *text = world_read(&w);

        for (i = 0; i < report.count; i++)
            absent += report.items[i].outcome == KEY_ABSENT ? 1 : 0;
        CHECK_NUM(absent, 1, "the setting that is not there is reported");
        CHECK_NUM(report.changed, 2, "and the other two still go in");
        CHECK(strstr(text, "input_p2_jump_key") == NULL,
              "nothing is invented for the one that is missing");
        free(text);
        json_free(record);
        keys_report_free(&report);
    }
    world_free(&w);
}

static void test_missing_file(void)
{
    world w;
    keys_plan plan;
    char err[TB_ERR_LEN];
    int players[2];

    test_case("a game with no bindings file at all is refused");
    world_build(&w, "keys_gone", NULL);
    players[0] = 1;
    players[1] = 2;

    err[0] = '\0';
    CHECK(keys_bind_build(&w.paths, players, 2, NULL, &plan, err, sizeof err) != 0,
          "there is nothing to bind");
    CHECK(err[0] != '\0', "and the reason says so: %s", err);
    keys_plan_free(&plan);

    err[0] = '\0';
    CHECK(keys_unbind_build(&w.paths, players, 2, NULL, &plan, err, sizeof err) != 0,
          "and nothing to restore either");
    keys_plan_free(&plan);
    world_free(&w);
}

static void test_undo(void)
{
    world w;
    keys_plan plan;
    keys_report report;
    char err[TB_ERR_LEN];
    int players[2];
    char *text;

    test_case("a bind that cannot be recorded is taken straight back out");
    world_build(&w, "keys_undo", KEYS_TABLE);
    players[0] = 1;
    players[1] = 2;

    if (CHECK(keys_bind_build(&w.paths, players, 2, NULL, &plan, err, sizeof err) == 0,
              "the binding is worked out (%s)", err)) {
        CHECK(keys_plan_apply(&plan, &report, err, sizeof err) == 0,
              "and written (%s)", err);
        text = world_read(&w);
        CHECK_STR(text, KEYS_P2_BOUND, "the keys changed");
        free(text);

        keys_plan_undo(&plan);
        text = world_read(&w);
        CHECK_STR(text, KEYS_TABLE, "and came straight back, byte for byte");
        free(text);
        keys_report_free(&report);
    }
    keys_plan_free(&plan);
    world_free(&w);
}

/* ---- The state record -------------------------------------------------- */

static void test_state_record(void)
{
    world w;
    config *cfg;
    const json_value *record;
    char err[TB_ERR_LEN];

    test_case("a recorded binding survives a trip through the state file");
    world_build(&w, "keys_state", KEYS_TABLE);

    cfg = config_load(err, sizeof err);
    CHECK(cfg != NULL, "the state file loads (%s)", cfg ? "" : err);
    if (cfg) {
        config_set_keybindings(cfg, record_of(P2_JUMP_KEY, P1_JUMP));
        CHECK(config_save(cfg, err, sizeof err) == 0, "and saves (%s)", err);
        config_free(cfg);

        cfg = config_load(err, sizeof err);
        record = cfg ? config_get_keybindings(cfg) : NULL;
        CHECK(record != NULL, "the record is read back");
        CHECK_STR(json_get_string(record, P2_JUMP_KEY, ""), P1_JUMP,
                  "with the quotes of the value intact");

        config_set_keybindings(cfg, NULL);
        record = config_get_keybindings(cfg);
        CHECK(record != NULL && json_count(record) == 0,
              "and unbinding leaves it empty rather than absent");
        config_free(cfg);
    }
    world_free(&w);
}

/* ---- Suite ------------------------------------------------------------- */

void suite_keys(void)
{
    test_suite("keys");
    test_players_parsed();
    test_setting_names();
    test_bind_one_player();
    test_bind_two_players();
    test_bind_what_already_matches();
    test_bind_twice_keeps_the_original();
    test_bind_needs_two_players();
    test_unbind_restores();
    test_unbind_clears_unrecorded();
    test_unbind_prefers_the_record();
    test_unbind_mixes_both();
    test_unbind_with_nothing_to_do();
    test_layout_is_kept();
    test_missing_settings();
    test_missing_file();
    test_undo();
    test_state_record();
}
