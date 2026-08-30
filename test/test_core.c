/*
 * test_core.c - Strings, paths, JSON, KeyValues and MD5.
 *
 * These cover the pieces every other suite leans on, including the cases that
 * bit us for real: UTF-8 column widths, Steam's lowercase registry paths, and
 * JSON round trips that must not lose keys.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "digest.h"
#include "json.h"
#include "kv.h"
#include "log.h"
#include "md5.h"
#include "platform.h"
#include "test.h"
#include "util.h"

/* The two Steam files the tool actually parses, as they appear on disk. */
static const char *VDF_SAMPLE =
"\"libraryfolders\"\n"
"{\n"
"\t\"0\"\n"
"\t{\n"
"\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
"\t\t\"label\"\t\t\"\"\n"
"\t\t\"apps\"\n"
"\t\t{\n"
"\t\t\t\"228980\"\t\t\"58639359\"\n"
"\t\t\t\"230270\"\t\t\"1025481560\"\n"
"\t\t}\n"
"\t}\n"
"\t\"1\"\n"
"\t{\n"
"\t\t\"path\"\t\t\"D:\\\\Games\\\\SteamLibrary\"\n"
"\t\t\"apps\" { }\n"
"\t}\n"
"}\n";

static const char *ACF_SAMPLE =
"\"AppState\"\n"
"{\n"
"\t\"appid\"\t\t\"230270\"\n"
"\t\"name\"\t\t\"N++\"\n"
"\t// a comment the parser must ignore\n"
"\t\"installdir\"\t\t\"N++\"\n"
"\t\"UserConfig\"\n"
"\t{\n"
"\t\t\"language\"\t\t\"english\"\n"
"\t}\n"
"}\n";

static void test_strings(void)
{
    char *lower;

    test_case("string helpers");
    CHECK(str_ieq("MET", "met"), "str_ieq is case-insensitive");
    CHECK(!str_ieq("met", "meta"), "str_ieq rejects a prefix");
    CHECK(str_ieq(NULL, NULL), "str_ieq treats two nulls as equal");
    CHECK(!str_ieq("a", NULL), "str_ieq rejects one null");

    lower = str_dup_lower("MeT99");
    CHECK_STR(lower, "met99", "str_dup_lower");
    free(lower);

    /* Column alignment counts characters, not bytes: this is the author name
     * that first exposed the difference. */
    CHECK_NUM(str_display_width("Filip"), 5, "width of plain ASCII");
    CHECK_NUM(str_display_width("Filip\xE2\x9C\x9D"), 6, "width counts U+271D once");
    CHECK_NUM(strlen("Filip\xE2\x9C\x9D"), 8, "...though it is 8 bytes");
}

/*
 * The two cells a tab listing does not show as stored. Both front-ends print
 * these, so a change to either shows up in the CLI and the GUI at once.
 */
static void test_tab_columns(void)
{
    char out[DIGEST_CODE_BUF];
    char date[DIGEST_DATE_BUF];
    char tight[3];

    test_case("tab columns");

    digest_code_upper(out, sizeof out, "ziv");
    CHECK_STR(out, "ZIV", "the code is shown as the game shows it");
    digest_code_upper(out, sizeof out, "ZIV");
    CHECK_STR(out, "ZIV", "one already upper is left alone");
    digest_code_upper(out, sizeof out, "nv1");
    CHECK_STR(out, "NV1", "a digit in it survives");
    digest_code_upper(out, sizeof out, NULL);
    CHECK_STR(out, "", "no code gives an empty cell, not a crash");

    /* Truncation must still terminate: these go straight into a printf. */
    digest_code_upper(tight, sizeof tight, "ziv");
    CHECK_STR(tight, "ZI", "a code too long for the buffer is cut, not run on");

    digest_date_short(date, sizeof date, "2015-07-30T00:00:00.000Z");
    CHECK_STR(date, "2015-07-30", "a timestamp keeps its date");
    digest_date_short(date, sizeof date, "2015-07-30");
    CHECK_STR(date, "2015-07-30", "a bare date is already what we want");
    CHECK_NUM(DIGEST_DATE_BUF, DIGEST_DATE_LEN + 1,
              "the buffer holds a whole date and its terminator");

    /* Anything shorter than a date is not one, so it is shown whole rather
     * than cut somewhere that would read as a different date. */
    digest_date_short(date, sizeof date, "2015-07");
    CHECK_STR(date, "2015-07", "something shorter is shown as it is");
    digest_date_short(date, sizeof date, "");
    CHECK_STR(date, "", "an empty date stays empty");
    digest_date_short(date, sizeof date, NULL);
    CHECK_STR(date, "", "no date gives an empty cell");
}

/*
 * Timestamps: the state file's own format in and out, and the shape the GUI
 * shows a date in. `now` is passed in rather than read, so these do not depend
 * on the clock or on the machine's timezone.
 */
static void test_timestamps(void)
{
    char out[TB_WHEN_LEN];
    char stamp[TB_TIMESTAMP_LEN + 1];
    char clock[TB_CLOCK_LEN], later[TB_CLOCK_LEN];
    long long now = 1787747696LL;   /* 2026-08-26T12:34:56Z */
    int i;

    test_case("timestamps");

    CHECK_NUM(time_from_iso8601("1970-01-01T00:00:00Z"), 0, "the epoch itself");
    CHECK_NUM(time_from_iso8601("2000-01-01T00:00:00Z"), 946684800L, "a round date");
    CHECK_NUM(time_from_iso8601("2015-07-30T00:00:00Z"), 1438214400L,
              "the day the first mappack is dated");
    CHECK_NUM(time_from_iso8601("2026-08-26T12:34:56Z"), 1787747696L,
              "a date with a time on it");
    /* Some digests stamp fractions of a second; the rest of the string is not
     * ours to read, so it is ignored rather than refused. */
    CHECK_NUM(time_from_iso8601("2015-07-30T00:00:00.000Z"), 1438214400L,
              "a fractional stamp parses the same");
    CHECK_NUM(time_from_iso8601(""), 0, "an empty string is not a date");
    CHECK_NUM(time_from_iso8601(NULL), 0, "neither is nothing at all");
    CHECK_NUM(time_from_iso8601("yesterday"), 0, "nor is prose");
    CHECK_NUM(time_from_iso8601("2015-13-30T00:00:00Z"), 0, "nor a 13th month");

    /* The two directions have to agree, or a date written by one version of
     * the tool would read as a different one to the next. */
    time_now_iso8601(stamp, sizeof stamp);
    CHECK(time_from_iso8601(stamp) > 1438214400L, "what we write, we can read back");

    time_relative(0, now, "Never", out, sizeof out);
    CHECK_STR(out, "Never", "no timestamp at all");
    time_relative(now + 3600, now, "Never", out, sizeof out);
    CHECK_STR(out, "Never", "a stamp from the future is a clock we cannot trust");
    time_relative(0, now, NULL, out, sizeof out);
    CHECK_STR(out, "", "and the stand-in may be nothing");

    time_relative(now, now, "Never", out, sizeof out);
    CHECK_STR(out, "just now", "this very second");
    time_relative(now - 59, now, "Never", out, sizeof out);
    CHECK_STR(out, "just now", "under a minute");
    time_relative(now - 60, now, "Never", out, sizeof out);
    CHECK_STR(out, "1 minute ago", "the singular has no s");
    time_relative(now - 150, now, "Never", out, sizeof out);
    CHECK_STR(out, "2 minutes ago", "minutes round down");
    time_relative(now - 3600, now, "Never", out, sizeof out);
    CHECK_STR(out, "1 hour ago", "an hour");
    time_relative(now - 86400, now, "Never", out, sizeof out);
    CHECK_STR(out, "1 day ago", "a day");
    time_relative(now - 3 * 86400, now, "Never", out, sizeof out);
    CHECK_STR(out, "3 days ago", "several days");
    time_relative(now - 45 * 86400, now, "Never", out, sizeof out);
    CHECK_STR(out, "1 month ago", "past a month, months");
    time_relative(now - 400 * 86400, now, "Never", out, sizeof out);
    CHECK_STR(out, "1 year ago", "and past a year, years");

    /* The clock a logged line is stamped with. Which hour it reads is the
     * machine's own business, so what is checked is its shape and that it
     * follows the moment it is given. */
    time_local_clock(now, clock, sizeof clock);
    CHECK_NUM(strlen(clock), TB_CLOCK_LEN - 1, "a clock fills its buffer exactly");
    CHECK(clock[2] == ':' && clock[5] == ':', "with the colons where they belong");
    for (i = 0; i < 8; i++) {
        if (i == 2 || i == 5)
            continue;
        if (clock[i] < '0' || clock[i] > '9') {
            CHECK(0, "and digits everywhere else");
            break;
        }
    }
    time_local_clock(now + 1, later, sizeof later);
    CHECK(strcmp(clock, later) != 0, "a second later reads as a second later");

    time_local_clock(0, clock, sizeof clock);
    CHECK_STR(clock, "--:--:--", "no moment at all reads as dashes, which line up");
}

static void test_buffers(void)
{
    byte_buf buf = {0};
    str_list list = {0};
    char *out;
    size_t len = 0;

    test_case("buffers and lists");
    buf_append(&buf, "abc", 3);
    buf_append(&buf, "", 0);
    buf_append(&buf, "de", 2);
    out = buf_finish(&buf, &len);
    CHECK_STR(out, "abcde", "buf_append then buf_finish");
    CHECK_NUM(len, 5, "buf_finish reports the length");
    free(out);

    str_list_push(&list, str_dup("beta"));
    str_list_push(&list, str_dup("alpha"));
    str_list_push(&list, NULL);            /* ignored, so failed lookups are safe */
    CHECK_NUM(list.count, 2, "str_list_push skips nulls");
    str_list_sort(&list);
    CHECK_STR(list.items[0], "alpha", "str_list_sort orders entries");
    CHECK(str_list_contains(&list, "ALPHA"), "str_list_contains ignores case");
    CHECK(!str_list_contains(&list, "gamma"), "str_list_contains rejects absent");
    str_list_free(&list);
    CHECK_NUM(list.count, 0, "str_list_free empties the list");
}

static void test_paths(void)
{
    char *joined, *dir;
    char native[64];

    test_case("path helpers");
    joined = path_join("a", "b");
    CHECK_STR(joined, "a" PATH_SEP_STR "b", "path_join");
    free(joined);

    joined = path_join("a" PATH_SEP_STR, PATH_SEP_STR "b");
    CHECK_STR(joined, "a" PATH_SEP_STR "b", "path_join never doubles separators");
    free(joined);

    dir = path_dirname("a" PATH_SEP_STR "b" PATH_SEP_STR "c.txt");
    CHECK_STR(dir, "a" PATH_SEP_STR "b", "path_dirname");
    free(dir);

    dir = path_dirname("plain.txt");
    CHECK_STR(dir, ".", "path_dirname of a bare name");
    free(dir);

    snprintf(native, sizeof native, "%s", "one/two/");
    path_to_native(native);
#ifdef _WIN32
    CHECK_STR(native, "one\\two", "path_to_native rewrites and trims separators");
    CHECK(path_is_absolute("C:\\x"), "drive letters are absolute");
    CHECK(path_is_absolute("\\\\server\\share"), "UNC paths are absolute");
    CHECK(!path_is_absolute("x\\y"), "relative paths are not");
#else
    CHECK_STR(native, "one/two", "path_to_native trims trailing separators");
    CHECK(path_is_absolute("/x"), "rooted paths are absolute");
    CHECK(!path_is_absolute("x/y"), "relative paths are not");
#endif
}

/*
 * Steam's HKCU key stores a lowercase, forward-slash path. Canonicalisation is
 * what turns that back into the spelling on disk.
 */
static void test_canonical(void)
{
    char *base = test_dir("canon");
    char *mixed = path_join(base, "MiXeD_Case");
    char *lowered, *canonical;

    test_case("canonical paths");
    plat_mkdir_p(mixed);

    lowered = str_dup_lower(mixed);
    canonical = plat_canonical_path(lowered);
    CHECK(canonical != NULL, "an existing directory canonicalises");
#ifdef _WIN32
    if (canonical)
        CHECK(strstr(canonical, "MiXeD_Case") != NULL,
              "canonical path restores the on-disk case (got '%s')", canonical);
#endif
    free(canonical);

    canonical = plat_canonical_path("no/such/path/anywhere");
    CHECK(canonical == NULL, "a missing path does not canonicalise");
    free(canonical);

    free(lowered);
    free(mixed);
    free(base);
}

/* Whether `s` ends with `tail`, for checking the shape of a path. */
static int ends_with(const char *s, const char *tail)
{
    size_t sl = strlen(s), tl = strlen(tail);
    return sl >= tl && strcmp(s + sl - tl, tail) == 0;
}

static void test_app_root(void)
{
    char *root = test_dir("approot");
    char *data = plat_data_dir();
    char *got;

    test_case("where the tool keeps its own files");

    /* The system's data directory, which app_root puts the tool's folder
     * inside of. Reading it changes nothing, so this is safe to ask for. */
    if (CHECK(data != NULL, "the system has a per-user data directory")) {
#ifdef _WIN32
        CHECK(data[0] != '\0' && (data[1] == ':' || data[0] == '\\'),
              "and names it absolutely (got '%s')", data);
#else
        CHECK(data[0] == '/', "and names it absolutely (got '%s')", data);
#endif
        CHECK(!ends_with(data, TABBER_DATA_DIRNAME),
              "without the tool's own folder already on the end (got '%s')", data);
    }

    /* TABBER_HOME overrides it outright, which is what keeps this very suite
     * out of the real one. */
    test_use_root(root);
    got = plat_app_root();
    CHECK_STR(got, root, "TABBER_HOME names the root outright");
    free(got);

    free(data);
    free(root);
}

static void test_move_entries(void)
{
    char *base = test_dir("adopt");
    char *from = path_join(base, "old");
    char *to = path_join(base, "new");
    char *src_file = path_join(from, "config.json");
    char *src_dir = path_join(from, "tabs");
    char *src_kept = path_join(from, "digest.json");
    char *dst_file = path_join(to, "config.json");
    char *dst_dir_file = path_join(to, "tabs/ziv/SI.txt");
    char *dst_kept = path_join(to, "digest.json");
    char *inner = path_join(from, "tabs/ziv/SI.txt");
    static const char *const names[] = { "config.json", "digest.json", "tabs" };
    size_t moved = 0;
    char *text;

    test_case("state is moved to the root without writing over any");

    plat_mkdir_p(to);
    test_write(src_file, "{ \"tabs\": [] }");
    test_write(inner, "levels");
    test_write(src_kept, "old digest");
    test_write(dst_kept, "the one already there");

    CHECK(plat_move_entries(from, to, names, 3, &moved) == 0,
          "the move reports success");
    CHECK_NUM(moved, 2, "the file and the directory moved, the third did not");

    text = test_read(dst_file);
    CHECK_STR(text, "{ \"tabs\": [] }", "the file arrived");
    free(text);
    text = test_read(dst_dir_file);
    CHECK_STR(text, "levels", "and so did the whole store, contents and all");
    free(text);

    /* The destination's own copy wins: an entry already there is never lost. */
    text = test_read(dst_kept);
    CHECK_STR(text, "the one already there", "what was already there is untouched");
    free(text);
    CHECK(plat_is_file(src_kept), "and the one that could not move is left behind");

    CHECK(!plat_is_file(src_file), "what moved is gone from the old place");
    CHECK(!plat_is_dir(src_dir), "the directory too");

    /* Running it again has nothing left to do, and says so. */
    moved = 0;
    CHECK(plat_move_entries(from, to, names, 3, &moved) == 0,
          "a second run succeeds");
    CHECK_NUM(moved, 0, "having moved nothing");

    free(base); free(from); free(to);
    free(src_file); free(src_dir); free(src_kept);
    free(dst_file); free(dst_dir_file); free(dst_kept); free(inner);
}

static void test_json_parsing(void)
{
    char err[TB_ERR_LEN];
    json_value *root;
    const json_value *tabs, *first;

    test_case("json parsing");
    root = json_parse(TEST_DIGEST_JSON, err, sizeof err);
    CHECK(root != NULL, "the digest fixture parses (%s)", root ? "" : err);
    if (!root)
        return;

    tabs = json_get(root, "tabs");
    CHECK_NUM(json_count(tabs), 3, "tab count");
    first = json_at(tabs, 0);
    CHECK_STR(json_get_string(json_get(first, "attributes"), "code", "?"), "tst",
              "nested lookup");
    CHECK_NUM(json_get_int(json_get(first, "attributes"), "id", -1), 0, "integer member");
    CHECK(json_get_bool(json_get(first, "attributes"), "enabled", 0), "boolean member");
    CHECK(json_get(root, "Tabs") == NULL, "member lookup is case-sensitive");
    json_free(root);

    /* Escapes, including a surrogate pair, must decode to UTF-8. */
    root = json_parse("{\"s\":\"q\\\"b\\\\s\\tt\\u271d\\ud83d\\ude00\"}", err, sizeof err);
    CHECK(root != NULL, "escapes parse (%s)", root ? "" : err);
    if (root) {
        CHECK_STR(json_get_string(root, "s", ""),
                  "q\"b\\s\tt\xE2\x9C\x9D\xF0\x9F\x98\x80", "escapes decode to UTF-8");
        json_free(root);
    }

    /* Malformed input must be rejected, not half-accepted. */
    CHECK(json_parse("{\"a\":1} trailing", err, sizeof err) == NULL, "trailing data rejected");
    CHECK(json_parse("{\"a\":", err, sizeof err) == NULL, "truncated object rejected");
    CHECK(json_parse("{\"a\":\"unterminated}", err, sizeof err) == NULL, "unterminated string rejected");
    CHECK(json_parse("[1,2,]", err, sizeof err) == NULL, "trailing comma rejected");
    CHECK(json_parse("nope", err, sizeof err) == NULL, "bare word rejected");
}

static void test_json_writing(void)
{
    char err[TB_ERR_LEN];
    json_value *root = json_new_object();
    json_value *arr = json_new_array();
    json_value *again;
    char *text;

    test_case("json writing");
    json_object_set(root, "first", json_new_string("one"));
    json_object_set(root, "flag", json_new_bool(1));
    json_object_set(root, "count", json_new_number(42));
    json_object_set(root, "nothing", json_new_null());
    json_array_append(arr, json_new_number(1));
    json_array_append(arr, json_new_string("two"));
    json_object_set(root, "list", arr);

    /* Replacing a member keeps its position, so files hold their shape. */
    json_object_set(root, "first", json_new_string("replaced"));

    text = json_serialize(root, 1);
    CHECK(strstr(text, "\"count\": 42") != NULL, "whole numbers write without a point");
    CHECK(strstr(text, "\"nothing\": null") != NULL, "nulls survive");

    again = json_parse(text, err, sizeof err);
    CHECK(again != NULL, "serialized output parses back (%s)", again ? "" : err);
    if (again) {
        const json_value *members = again->children;
        CHECK_STR(json_get_string(again, "first", ""), "replaced", "member replaced in place");
        CHECK_STR(members ? members->key : NULL, "first", "replacement kept its position");
        CHECK_NUM(json_get_int(again, "count", 0), 42, "number round trip");
        CHECK_NUM(json_count(json_get(again, "list")), 2, "array round trip");
        json_free(again);
    }
    free(text);

    /* Text with quotes, backslashes, control characters and UTF-8. */
    json_object_set(root, "tricky", json_new_string("a\"b\\c\td\ne\xE2\x9C\x9D"));
    text = json_serialize(root, 0);
    again = json_parse(text, err, sizeof err);
    CHECK(again != NULL, "escaped text re-parses");
    if (again) {
        CHECK_STR(json_get_string(again, "tricky", ""), "a\"b\\c\td\ne\xE2\x9C\x9D",
                  "escaping round trips byte for byte");
        json_free(again);
    }
    free(text);
    json_free(root);
}

static void test_kv(void)
{
    char err[TB_ERR_LEN];
    kv_node *root;
    const kv_node *libs, *zero, *apps;

    test_case("keyvalues parsing");
    root = kv_parse_string(VDF_SAMPLE, err, sizeof err);
    CHECK(root != NULL, "libraryfolders.vdf parses (%s)", root ? "" : err);
    if (root) {
        libs = kv_child(root, "LIBRARYFOLDERS");   /* lookups ignore case */
        CHECK(libs != NULL, "top-level key found case-insensitively");
        zero = kv_child(libs, "0");
        CHECK_STR(kv_value(zero, "path"), "C:\\Program Files (x86)\\Steam",
                  "backslash escapes decode");
        apps = kv_child(zero, "apps");
        CHECK(kv_child(apps, "230270") != NULL, "app id found in the apps block");
        CHECK(kv_child(apps, "999999") == NULL, "absent app id not found");
        CHECK(kv_child(libs, "1") != NULL, "second library parsed");
        kv_free(root);
    }

    root = kv_parse_string(ACF_SAMPLE, err, sizeof err);
    CHECK(root != NULL, "appmanifest parses (%s)", root ? "" : err);
    if (root) {
        CHECK_STR(kv_value(kv_child(root, "AppState"), "installdir"), "N++",
                  "installdir read past a comment");
        kv_free(root);
    }

    CHECK(kv_parse_string("\"a\" {", err, sizeof err) == NULL, "unclosed block rejected");
    CHECK(kv_parse_string("\"a\"", err, sizeof err) == NULL, "key without a value rejected");
}

static void test_md5(void)
{
    char hex[MD5_HEX_LEN + 1];

    /* The vectors from RFC 1321, plus a block-boundary case. */
    test_case("md5");
    md5_hex("", 0, hex);
    CHECK_STR(hex, "d41d8cd98f00b204e9800998ecf8427e", "md5 of the empty string");
    md5_hex("abc", 3, hex);
    CHECK_STR(hex, "900150983cd24fb0d6963f7d28e17f72", "md5 of 'abc'");
    md5_hex("message digest", 14, hex);
    CHECK_STR(hex, "f96b697d7cb7938d525a2f31aaf161d0", "md5 of 'message digest'");
    md5_hex("12345678901234567890123456789012345678901234567890"
            "123456789012345678901234567890", 80, hex);
    CHECK_STR(hex, "57edf4a22be3c955ac49da2e2107b67a", "md5 across block boundaries");
}

/*
 * The message log. Everything here runs with the echo off: what the tool has
 * to say would otherwise land in the middle of what the suite has to say.
 */
static void test_log(void)
{
    char long_line[LOG_LINE_MAX * 2];
    const log_entry *entry;
    long long before, after;
    size_t i;

    test_case("the message log");
    log_set_echo(0);
    log_clear();

    CHECK(log_last() == NULL, "a log nothing has been written to is empty");
    CHECK_NUM(log_count(), 0, "...and counts nothing");
    CHECK(log_at(0) == NULL, "...and has no newest line");

    before = (long long)time(NULL);
    log_line("Installing %s (%s)...", "MET", "Metanet");
    after = (long long)time(NULL);
    entry = log_last();
    CHECK(entry != NULL, "a line is recorded");
    if (!entry) {
        log_set_echo(1);
        return;
    }
    CHECK_STR(entry->text, "Installing MET (Metanet)...", "...formatted as it was given");
    CHECK(entry->when >= before && entry->when <= after, "...and stamped with the time");
    CHECK_NUM(log_count(), 1, "one line is being kept");

    /* Several lines, and what the log calls the newest of them. */
    log_line("MET installed successfully.");
    CHECK_STR(log_at(0)->text, "MET installed successfully.", "the newest is age 0");
    CHECK_STR(log_at(1)->text, "Installing MET (Metanet)...", "the one before it is age 1");
    CHECK(log_at(2) == NULL, "and there is no third");

    /* A dialog's worth of text becomes one line, which is what the bar at the
     * bottom of the window has room for. */
    log_line("MET is installed, and only one custom tab can be installed at a "
             "time.\n\nUninstall MET and install LIT in its place?");
    CHECK_STR(log_last()->text,
              "MET is installed, and only one custom tab can be installed at a "
              "time. Uninstall MET and install LIT in its place?",
              "several lines are folded onto one");

    log_line("  \t spaced \n out \r\n ");
    CHECK_STR(log_last()->text, "spaced out", "runs of whitespace collapse and the ends go");

    log_line("   ");
    CHECK_STR(log_last()->text, "spaced out", "a line with nothing in it is not recorded");

    /* Longer than a line may be, and cut with the ellipsis in its place. */
    memset(long_line, 'x', sizeof long_line - 1);
    long_line[sizeof long_line - 1] = '\0';
    log_line("%s", long_line);
    entry = log_last();
    CHECK_NUM(strlen(entry->text), LOG_LINE_MAX - 1, "an over-long line is cut to fit");
    CHECK_STR(entry->text + LOG_LINE_MAX - 1 - strlen(LOG_ELLIPSIS), LOG_ELLIPSIS,
              "...and says so with an ellipsis");

    /* The oldest fall off the end rather than the newest being refused. */
    log_clear();
    for (i = 0; i < LOG_HISTORY + 10; i++)
        log_line("line %u", (unsigned)i);
    CHECK_NUM(log_count(), LOG_HISTORY, "the log stops growing at LOG_HISTORY");
    CHECK_STR(log_at(0)->text, "line 137", "the newest line is the last one written");
    CHECK_STR(log_at(LOG_HISTORY - 1)->text, "line 10", "and the oldest kept is that far back");
    CHECK(log_at(LOG_HISTORY) == NULL, "nothing older than that survives");

    log_clear();
    CHECK_NUM(log_count(), 0, "clearing empties it again");
    log_set_echo(1);
}

/*
 * The same log, kept on disk. Every line is appended to one file in the tool's
 * folder, dated as well as timed, because that file holds more than one run.
 */
static void test_log_file(void)
{
    char *root = test_dir("logfile");
    char *path, *text, *old, *big;
    size_t i;

    test_case("the log on disk");
    test_use_root(root);
    log_set_echo(0);
    log_clear();

    path = log_file_path();
    if (!CHECK(path != NULL, "the file has a place in the tool's folder")) {
        log_set_echo(1);
        free(root);
        return;
    }
    CHECK(!plat_is_file(path), "and nothing is written there until it is asked for");

    /* Off by default: a run that does not ask leaves no trace. */
    log_line("Fetching MET (Metanet)...");
    CHECK(!plat_is_file(path), "a line logged with saving off does not make one");

    log_set_saving(1);
    log_line("MET fetched successfully.");
    text = plat_read_file(path, NULL);
    if (CHECK(text != NULL, "the first line saved makes the file")) {
        CHECK(strstr(text, "MET fetched successfully.") != NULL, "with the line in it");
        CHECK_NUM(strlen(text), TB_STAMP_LEN - 1 + 2 +
                  strlen("MET fetched successfully.") + 1,
                  "a stamp, two spaces, the line and a newline");
        CHECK(text[4] == '-' && text[7] == '-' && text[10] == ' ' &&
              text[13] == ':' && text[16] == ':',
              "the stamp carries the date as well as the time (got '%.19s')", text);
        CHECK(strstr(text, "Fetching MET") == NULL,
              "and the line logged before saving was on is not in it");
        free(text);
    }

    /* Appended to, not written over: the file is the account of every run. */
    log_line("Installing MET (Metanet)...");
    text = plat_read_file(path, NULL);
    if (CHECK(text != NULL, "the file survives a second line")) {
        CHECK(strstr(text, "MET fetched successfully.") != NULL, "the first is still there");
        CHECK(strstr(text, "Installing MET (Metanet)...") != NULL, "and the second is after it");
        free(text);
    }

    log_set_saving(0);
    log_line("MET installed successfully.");
    text = plat_read_file(path, NULL);
    if (CHECK(text != NULL, "switching it off leaves what is already written")) {
        CHECK(strstr(text, "MET installed successfully.") == NULL,
              "but adds nothing more to it");
        free(text);
    }

    /* Past the cap, the file is set aside and a fresh one started, so two
     * files' worth is the most the folder ever holds. */
    big = (char *)xmalloc(LOG_FILE_MAX);
    memset(big, 'x', LOG_FILE_MAX);
    CHECK_NUM(test_write_bytes(path, big, LOG_FILE_MAX), 0, "a file at the cap");
    free(big);
    old = str_fmt("%s%s", path, LOG_OLD_SUFFIX);
    log_set_saving(1);
    log_line("Uninstalling MET (Metanet)...");
    CHECK(plat_is_file(old), "one line past the cap sets the file aside");
    text = plat_read_file(path, NULL);
    CHECK(text == NULL, "and the next line starts a fresh one");
    free(text);
    text = plat_read_file(old, NULL);
    if (CHECK(text != NULL, "what was set aside is the file that was too big")) {
        CHECK(strstr(text, "Uninstalling MET (Metanet)...") != NULL,
              "with the line that tipped it over on the end");
        free(text);
    }

    /* And only ever one of them: a second roll-over replaces the first. */
    for (i = 0; i < 2; i++) {
        big = (char *)xmalloc(LOG_FILE_MAX);
        memset(big, 'x', LOG_FILE_MAX);
        test_write_bytes(path, big, LOG_FILE_MAX);
        free(big);
        log_line("Removing MET...");
    }
    text = plat_read_file(old, NULL);
    if (CHECK(text != NULL, "the one before it is kept")) {
        CHECK(strstr(text, "Removing MET...") != NULL, "and is the one just set aside");
        free(text);
    }

    log_set_saving(0);
    log_clear();
    log_set_echo(1);
    free(old);
    free(path);
    free(root);
}

void suite_core(void)
{
    test_suite("core");
    test_strings();
    test_log();
    test_log_file();
    test_tab_columns();
    test_timestamps();
    test_buffers();
    test_paths();
    test_canonical();
    test_app_root();
    test_move_entries();
    test_json_parsing();
    test_json_writing();
    test_kv();
    test_md5();
}
