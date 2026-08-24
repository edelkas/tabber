/*
 * test_save.c - Archiving and swapping N++'s savefile.
 *
 * The savefile is the one thing tabber touches that cannot be recovered from
 * Steam, so these tests care less about the happy path than about what is left
 * behind when something goes wrong: after every refusal the save that was
 * there must still be there, byte for byte.
 *
 * Everything runs against a stand-in personal folder in scratch space.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cloud.h"
#include "gzip.h"
#include "paths.h"
#include "platform.h"
#include "resource.h"
#include "save.h"
#include "test.h"
#include "util.h"
#include "zip.h"

#define TAB_CODE      "tst"
#define TAB_ARCHIVE   "nprofile_tst.zip"

/* Stand-in saves. Their contents are opaque to tabber, so any bytes will do. */
#define VANILLA_SAVE  "vanilla save, 100% completed"
#define TAB_SAVE      "the tab's own save, half played"
#define FRESH_SAVE    "the fresh save tabber ships"

/* A personal folder, the paths struct pointing at it, and the shipped save. */
typedef struct {
    char *root;
    char *dir;
    char *fresh;
    npp_paths paths;
} world;

/*
 * Builds the world: a personal folder holding `save_name` (NULL for none) and
 * a shipped fresh archive holding an uncompressed save.
 */
static void world_build(world *w, const char *name, const char *save_name,
                        const void *save, size_t len)
{
    memset(w, 0, sizeof(*w));
    w->root = test_dir(name);
    test_use_root(w->root);
    w->dir = test_fake_personal(w->root, save_name, save, len);
    w->paths.personal_dir = w->dir;

    w->fresh = path_join(w->root, "res_nprofile.zip");
    test_write_zip(w->fresh, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE));
    test_use_fresh_save(w->fresh);
}

static void world_free(world *w)
{
    test_use_fresh_save(NULL);
    free(w->root);
    free(w->dir);
    free(w->fresh);
}

/* Path of a file in the personal folder. Caller frees. */
static char *at(world *w, const char *name)
{
    return path_join(w->dir, name);
}

/* Contents of a file in the personal folder, with its length. Caller frees. */
static unsigned char *slurp(world *w, const char *name, size_t *len_out)
{
    char *path = at(w, name);
    unsigned char *data = test_read_bytes(path, len_out);

    free(path);
    return data;
}

static int exists(world *w, const char *name)
{
    char *path = at(w, name);
    int there = plat_is_file(path);

    free(path);
    return there;
}

/* Whether a file holds exactly these bytes. */
static int holds(world *w, const char *name, const void *want, size_t want_len)
{
    size_t len = 0;
    unsigned char *data = slurp(w, name, &len);
    int same = data && len == want_len && memcmp(data, want, want_len) == 0;

    free(data);
    return same;
}

/* Reads the savefile out of an archive in the personal folder. Caller frees. */
static unsigned char *archived(world *w, const char *name, size_t *len_out)
{
    char err[TB_ERR_LEN];
    char *path = at(w, name);
    unsigned char *raw, *save = NULL;
    size_t raw_len = 0;
    zip_archive zip;
    const zip_entry *entry;

    *len_out = 0;
    raw = test_read_bytes(path, &raw_len);
    free(path);
    if (!raw)
        return NULL;
    if (zip_open(&zip, raw, raw_len, err, sizeof err) == 0) {
        entry = zip_find_prefix(&zip, SAVE_ENTRY_PREFIX);
        if (entry) {
            save = zip_read(&zip, entry, err, sizeof err);
            *len_out = entry->uncomp_size;
        }
        zip_close(&zip);
    }
    free(raw);
    return save;
}

/* The name of the single entry in an archive. Caller frees. */
static char *archived_entry_name(world *w, const char *name)
{
    char err[TB_ERR_LEN];
    char *path = at(w, name);
    unsigned char *raw;
    size_t raw_len = 0;
    zip_archive zip;
    char *entry_name = NULL;

    raw = test_read_bytes(path, &raw_len);
    free(path);
    if (!raw)
        return NULL;
    if (zip_open(&zip, raw, raw_len, err, sizeof err) == 0) {
        if (zip.count == 1)
            entry_name = str_dup(zip.entries[0].name);
        zip_close(&zip);
    }
    free(raw);
    return entry_name;
}

/* Runs a whole swap, reporting what went wrong if it did. */
static int swap_with(world *w, int installing, unsigned flags, save_report *report)
{
    char err[TB_ERR_LEN];
    save_plan plan;
    int rc;

    if (save_plan_build(&w->paths, TAB_CODE, installing, flags, &plan, err, sizeof err) != 0) {
        CHECK(0, "the swap could not be planned: %s", err);
        return -1;
    }
    rc = save_plan_apply(&plan, report, err, sizeof err);
    CHECK(rc == 0, "the swap applied (%s)", err);
    save_plan_free(&plan);
    return rc;
}

static int swap(world *w, int installing, save_report *report)
{
    return swap_with(w, installing, 0, report);
}

/* ---- The forms the savefile comes in ----------------------------------- */

/*
 * A TEN++ game: the save is gzipped. What goes in is the uncompressed save
 * tabber ships, so it is written uncompressed and the gzipped one has to go,
 * or the game would read that instead.
 */
static void test_install_over_gzipped(void)
{
    world w;
    save_report report;
    unsigned char *gz, *kept;
    size_t gz_len = 0, kept_len = 0;
    char *entry;

    test_case("installing over a gzipped save");
    gz = test_gzip(VANILLA_SAVE, strlen(VANILLA_SAVE), &gz_len);
    world_build(&w, "save_gz", SAVE_GZ_NAME, gz, gz_len);

    if (swap(&w, 1, &report) == 0) {
        CHECK(report.backed_up, "the save in place was archived");
        CHECK(report.used_fresh, "the shipped save was used, as this tab has none yet");

        /* The archive holds the gzipped file, under its own name. */
        entry = archived_entry_name(&w, SAVE_BACKUP_ORIGINAL);
        CHECK_STR(entry, SAVE_GZ_NAME, "the archive names the file it holds");
        free(entry);
        kept = archived(&w, SAVE_BACKUP_ORIGINAL, &kept_len);
        CHECK(kept && kept_len == gz_len && memcmp(kept, gz, gz_len) == 0,
              "and holds it byte for byte");
        free(kept);

        /* The new save is there, uncompressed, and alone. */
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)),
              "the fresh save is in place, uncompressed");
        CHECK(!exists(&w, SAVE_GZ_NAME), "the gzipped save is gone, not shadowing it");
        CHECK_NUM(report.gzipped, 0, "and the report says so");
    }

    free(gz);
    world_free(&w);
}

/*
 * The same game, but what goes in is gzipped too. Then it stays gzipped: the
 * game has proven it reads that form by having one.
 */
static void test_gzipped_stays_gzipped(void)
{
    world w;
    save_report report;
    unsigned char *live, *tab;
    size_t live_len = 0, tab_len = 0;
    char *archive;

    test_case("a gzipped save that stays gzipped");
    live = test_gzip(VANILLA_SAVE, strlen(VANILLA_SAVE), &live_len);
    tab = test_gzip(TAB_SAVE, strlen(TAB_SAVE), &tab_len);
    world_build(&w, "save_gz_gz", SAVE_GZ_NAME, live, live_len);

    /* This tab has been played before, and its save was archived gzipped. */
    archive = at(&w, TAB_ARCHIVE);
    test_write_zip(archive, SAVE_GZ_NAME, tab, tab_len);
    free(archive);

    if (swap(&w, 1, &report) == 0) {
        CHECK(!report.used_fresh, "the tab's own save was used");
        CHECK(holds(&w, SAVE_GZ_NAME, tab, tab_len), "it is in place, still gzipped");
        CHECK(!exists(&w, SAVE_NAME), "no uncompressed copy was left behind");
        CHECK_NUM(report.gzipped, 1, "the report says it was written gzipped");
    }

    free(live);
    free(tab);
    world_free(&w);
}

/*
 * A game from before TEN++: the save is uncompressed, so the build may not
 * understand gzip at all. A gzipped save must be unwrapped before it is
 * handed over, never written as it is.
 */
static void test_old_build_never_gets_gzip(void)
{
    world w;
    save_report report;
    unsigned char *tab;
    size_t tab_len = 0;
    char *archive;

    test_case("an old build is never handed a gzipped save");
    tab = test_gzip(TAB_SAVE, strlen(TAB_SAVE), &tab_len);
    world_build(&w, "save_raw", SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE));

    archive = at(&w, TAB_ARCHIVE);
    test_write_zip(archive, SAVE_GZ_NAME, tab, tab_len);
    free(archive);

    if (swap(&w, 1, &report) == 0) {
        CHECK(holds(&w, SAVE_NAME, TAB_SAVE, strlen(TAB_SAVE)),
              "the gzipped save was unwrapped first");
        CHECK(!exists(&w, SAVE_GZ_NAME), "and no gzipped file was created");
        CHECK_NUM(report.gzipped, 0, "the report says so");
    }

    free(tab);
    world_free(&w);
}

/*
 * Both forms on disk at once, which is what an old build leaves behind after
 * TEN++ has written its first gzipped save. The gzipped one is the live save,
 * and the stale uncompressed one must not be the thing that gets archived.
 */
static void test_both_forms_present(void)
{
    world w;
    save_report report;
    unsigned char *gz, *kept;
    size_t gz_len = 0, kept_len = 0;
    char *path;

    test_case("both a gzipped and an uncompressed save");
    gz = test_gzip(VANILLA_SAVE, strlen(VANILLA_SAVE), &gz_len);
    world_build(&w, "save_both", SAVE_GZ_NAME, gz, gz_len);

    path = at(&w, SAVE_NAME);
    test_write_bytes(path, "stale, from before the game started gzipping", 43);
    free(path);

    if (swap(&w, 1, &report) == 0) {
        kept = archived(&w, SAVE_BACKUP_ORIGINAL, &kept_len);
        CHECK(kept && kept_len == gz_len && memcmp(kept, gz, gz_len) == 0,
              "the gzipped save is the one that was archived");
        free(kept);
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)),
              "the stale copy was overwritten by the new save");
        CHECK(!exists(&w, SAVE_GZ_NAME), "and the gzipped one is gone");
    }

    free(gz);
    world_free(&w);
}

/* A game that has never been run: nothing to archive, and no proof of gzip. */
static void test_no_save_at_all(void)
{
    world w;
    save_report report;

    test_case("a personal folder with no savefile");
    world_build(&w, "save_none", NULL, NULL, 0);

    if (swap(&w, 1, &report) == 0) {
        CHECK(!report.backed_up, "nothing was archived");
        CHECK(!exists(&w, SAVE_BACKUP_ORIGINAL), "and no archive was written");
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)),
              "the fresh save is in place, uncompressed");
        CHECK(!exists(&w, SAVE_GZ_NAME), "gzipped only when the game has shown it can");
    }

    world_free(&w);
}

/*
 * The save tabber ships is gzipped, and a game with no savefile has shown
 * nothing about what it can read, so it has to be unwrapped before it is put
 * in place. This is what a first-ever install looks like on an old build.
 */
static void test_fresh_save_gzipped(void)
{
    world w;
    save_report report;
    unsigned char *gz;
    size_t gz_len = 0;

    test_case("a gzipped shipped save on a game with none");
    world_build(&w, "save_fresh_gz", NULL, NULL, 0);

    /* Replace the fixture's shipped save with a gzipped one, as res/ holds. */
    gz = test_gzip(FRESH_SAVE, strlen(FRESH_SAVE), &gz_len);
    test_write_zip(w.fresh, SAVE_GZ_NAME, gz, gz_len);

    if (swap(&w, 1, &report) == 0) {
        CHECK(report.used_fresh, "the shipped save was used");
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)),
              "and put in place unwrapped");
        CHECK(!exists(&w, SAVE_GZ_NAME), "with no gzipped file created");
        CHECK_NUM(report.gzipped, 0, "as the report says");
    }

    free(gz);
    world_free(&w);
}

/*
 * With --force-compress the uncompressed save is gzipped on the way in rather
 * than left for the game to fall back to — but only where the default rule
 * would have allowed a gzipped save at all, which is to say where the game has
 * one of its own.
 */
static void test_force_compress(void)
{
    char err[TB_ERR_LEN];
    world w;
    save_report report;
    unsigned char *gz, *written, *plain;
    size_t gz_len = 0, written_len = 0, plain_len = 0;

    test_case("compressing the save on the way in");
    gz = test_gzip(VANILLA_SAVE, strlen(VANILLA_SAVE), &gz_len);
    world_build(&w, "save_force", SAVE_GZ_NAME, gz, gz_len);

    if (swap_with(&w, 1, SAVE_FORCE_COMPRESS, &report) == 0) {
        CHECK_NUM(report.gzipped, 1, "the save went in gzipped");
        CHECK_NUM(report.compressed, 1, "and tabber was the one that did it");
        CHECK(!exists(&w, SAVE_NAME), "no uncompressed copy was left behind");

        /* What landed has to be a gzip stream holding the save itself. */
        written = slurp(&w, SAVE_GZ_NAME, &written_len);
        CHECK(written && gz_is_gzip(written, written_len), "the file really is gzip");
        plain = written ? gz_extract(written, written_len, &plain_len, err, sizeof err) : NULL;
        CHECK(plain != NULL, "which unwraps (%s)", err);
        CHECK(plain && plain_len == strlen(FRESH_SAVE) &&
              memcmp(plain, FRESH_SAVE, plain_len) == 0,
              "to exactly the save that was meant to go in");
        free(plain);
        free(written);
    }
    world_free(&w);

    /* An old build has shown nothing, so the flag must not tempt us. */
    world_build(&w, "save_force_raw", SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE));
    if (swap_with(&w, 1, SAVE_FORCE_COMPRESS, &report) == 0) {
        CHECK_NUM(report.gzipped, 0, "an uncompressed save stays uncompressed");
        CHECK_NUM(report.compressed, 0, "nothing was compressed");
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)), "and it is in place");
        CHECK(!exists(&w, SAVE_GZ_NAME), "with no gzipped file invented");
    }
    world_free(&w);

    /* Neither must it re-compress what is already compressed. */
    world_build(&w, "save_force_gz", SAVE_GZ_NAME, gz, gz_len);
    {
        char *archive = at(&w, TAB_ARCHIVE);
        unsigned char *tab = test_gzip(TAB_SAVE, strlen(TAB_SAVE), &plain_len);

        test_write_zip(archive, SAVE_GZ_NAME, tab, plain_len);
        free(archive);
        if (swap_with(&w, 1, SAVE_FORCE_COMPRESS, &report) == 0) {
            CHECK_NUM(report.compressed, 0, "a gzipped save is passed through as it is");
            CHECK(holds(&w, SAVE_GZ_NAME, tab, plain_len), "byte for byte");
        }
        free(tab);
    }
    world_free(&w);

    free(gz);
}

/* ---- Round trips ------------------------------------------------------- */

/*
 * Install then uninstall: the vanilla save comes back exactly as it was, and
 * the tab's own progress is filed away for the next time it goes in.
 */
static void test_round_trip(void)
{
    world w;
    save_report report;
    unsigned char *gz, *back, *played;
    size_t gz_len = 0, back_len = 0, played_len = 0;
    char *path;

    test_case("install, play, uninstall, install again");
    gz = test_gzip(VANILLA_SAVE, strlen(VANILLA_SAVE), &gz_len);
    world_build(&w, "save_round", SAVE_GZ_NAME, gz, gz_len);

    if (swap(&w, 1, &report) != 0) { free(gz); world_free(&w); return; }

    /* The game is played: it rewrites the save, gzipped, as TEN++ does. */
    played = test_gzip(TAB_SAVE, strlen(TAB_SAVE), &played_len);
    path = at(&w, SAVE_GZ_NAME);
    test_write_bytes(path, played, played_len);
    free(path);
    path = at(&w, SAVE_NAME);
    plat_remove_file(path);          /* the game only writes the gzipped one */
    free(path);

    if (swap(&w, 0, &report) == 0) {
        CHECK(holds(&w, SAVE_GZ_NAME, gz, gz_len),
              "the vanilla save is back, byte for byte");
        CHECK(plat_is_file(report.backup_path), "the tab's save was archived");
        back = archived(&w, TAB_ARCHIVE, &back_len);
        CHECK(back != NULL, "under the name the old installers used");
        free(back);
    }

    /* Installing again must bring the tab's own progress back, not the fresh
     * save: that is the whole point of keeping the archive. */
    if (swap(&w, 1, &report) == 0) {
        CHECK(!report.used_fresh, "the second install used the tab's archive");
        CHECK(holds(&w, SAVE_GZ_NAME, played, played_len),
              "so the playthrough is back, byte for byte");
    }

    free(played);
    free(gz);
    world_free(&w);
}

/* Uninstalling without a vanilla archive falls back to the shipped save. */
static void test_uninstall_without_original(void)
{
    world w;
    save_report report;

    test_case("uninstalling with no archived vanilla save");
    world_build(&w, "save_no_og", SAVE_NAME, TAB_SAVE, strlen(TAB_SAVE));

    if (swap(&w, 0, &report) == 0) {
        CHECK(report.used_fresh, "the shipped save stands in for the missing one");
        CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)), "and is in place");
        CHECK(plat_is_file(report.backup_path), "the tab's save was archived first");
    }

    world_free(&w);
}

/* ---- Refusals, none of which may touch the savefile -------------------- */

static void test_refusals(void)
{
    char err[TB_ERR_LEN];
    world w;
    save_plan plan;
    char *archive, *path;

    test_case("a swap that cannot go ahead");
    world_build(&w, "save_refuse", SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE));

    /* An archive that is not an archive. */
    archive = at(&w, TAB_ARCHIVE);
    test_write(archive, "this is not a zip file, it is a lie");
    CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) != 0,
          "a corrupt archive is refused");
    CHECK(holds(&w, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE)),
          "and the savefile is untouched");
    CHECK(!exists(&w, SAVE_BACKUP_ORIGINAL), "nothing was archived either");
    plat_remove_file(archive);
    free(archive);

    /* An archive holding something else entirely. */
    archive = at(&w, TAB_ARCHIVE);
    test_write_zip(archive, "readme.txt", "not a save", 10);
    CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) != 0,
          "an archive with no savefile in it is refused");
    CHECK(strstr(err, SAVE_ENTRY_PREFIX) != NULL, "and says what it looked for");
    CHECK(holds(&w, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE)),
          "the savefile is still untouched");
    plat_remove_file(archive);
    free(archive);

    /* A savefile of zero bytes: something is wrong, so nothing is touched. */
    path = at(&w, SAVE_NAME);
    test_write_bytes(path, "", 0);
    CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) != 0,
          "an empty savefile stops the swap");
    CHECK(!exists(&w, SAVE_BACKUP_ORIGINAL), "nothing was archived");
    test_write_bytes(path, VANILLA_SAVE, strlen(VANILLA_SAVE));
    free(path);

    /*
     * No archive for this direction and no fresh save on disk either: the one
     * built into the executable is used, which is also the only check that it
     * really is a ZIP with a savefile in it.
     */
    test_use_fresh_save(NULL);
    if (CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) == 0,
              "with no save on disk, the built-in one is used (%s)", err)) {
        CHECK(plan.used_fresh && plan.from_builtin, "and is reported as built in");
        CHECK_STR(plan.source_path, SAVE_FRESH_BUILTIN, "named rather than pathed");
        CHECK(plan.save_len > 0, "a savefile came out of it");
        save_plan_free(&plan);
    }
    CHECK(holds(&w, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE)),
          "the savefile survived all of that, nothing having been applied");
    test_use_fresh_save(w.fresh);

    /* A personal folder that is not there. */
    {
        npp_paths nowhere = {0};
        char *missing = path_join(w.root, "not-a-folder");

        nowhere.personal_dir = missing;
        CHECK(save_plan_build(&nowhere, TAB_CODE, 1, 0, &plan, err, sizeof err) != 0,
              "a missing personal folder is refused");
        free(missing);
    }

    world_free(&w);
}

/* A swap that has to be taken back, because a later step of an install failed. */
static void test_undo(void)
{
    char err[TB_ERR_LEN];
    world w;
    save_report report;
    save_plan plan;

    test_case("undoing a swap");
    world_build(&w, "save_undo", SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE));

    CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) == 0,
          "the swap plans (%s)", err);
    CHECK(save_plan_apply(&plan, &report, err, sizeof err) == 0, "and applies (%s)", err);
    CHECK(holds(&w, SAVE_NAME, FRESH_SAVE, strlen(FRESH_SAVE)), "the new save is in place");

    CHECK(save_plan_undo(&plan) == 0, "the swap is undone");
    CHECK(holds(&w, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE)),
          "the savefile is exactly as it was");
    save_plan_free(&plan);

    /* Undoing a swap that put a save where there was none leaves none. */
    world_free(&w);
    world_build(&w, "save_undo_none", NULL, NULL, 0);
    CHECK(save_plan_build(&w.paths, TAB_CODE, 1, 0, &plan, err, sizeof err) == 0,
          "the swap plans with no save in place");
    CHECK(save_plan_apply(&plan, &report, err, sizeof err) == 0, "and applies");
    CHECK(save_plan_undo(&plan) == 0, "and is undone");
    CHECK(!exists(&w, SAVE_NAME), "leaving the folder as empty as it was");
    save_plan_free(&plan);

    world_free(&w);
}

/* ---- What the previous installers left behind -------------------------- */

/*
 * The archives the old Ruby installers wrote are what tabber has to keep
 * working with: a plain ZIP whose single entry is named after the savefile.
 */
static void test_old_installer_archives(void)
{
    world w;
    save_report report;
    char *archive, *entry;

    test_case("archives from the previous installers");
    world_build(&w, "save_legacy", SAVE_NAME, TAB_SAVE, strlen(TAB_SAVE));

    /* Its vanilla archive, from before saves were gzipped. */
    archive = at(&w, SAVE_BACKUP_ORIGINAL);
    test_write_zip(archive, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE));
    free(archive);

    if (swap(&w, 0, &report) == 0) {
        CHECK(!report.used_fresh, "the old archive is read as it is");
        CHECK(holds(&w, SAVE_NAME, VANILLA_SAVE, strlen(VANILLA_SAVE)),
              "and the vanilla save comes back out of it");
        entry = archived_entry_name(&w, TAB_ARCHIVE);
        CHECK_STR(entry, SAVE_NAME, "what tabber writes has the same shape");
        free(entry);
    }

    world_free(&w);
}

/* The save tabber ships has to be a real archive with a real save in it. */
static void test_shipped_save(void)
{
    char err[TB_ERR_LEN];
    const unsigned char *raw;
    unsigned char *save;
    size_t raw_len;
    zip_archive zip;
    const zip_entry *entry;

    test_case("the savefile tabber ships");
    test_use_fresh_save(NULL);

    /*
     * Straight out of the executable, where it now lives. There is no file to
     * go looking for, so there is nothing to skip either: this used to hunt
     * for res/nprofile.zip beside the binary and quietly test nothing when a
     * tree was built without it.
     */
    raw = RES_FRESH_SAVE;
    raw_len = RES_FRESH_SAVE_LEN;
    CHECK(raw_len > 0, "it is built into the binary (%lu bytes)",
          (unsigned long)raw_len);
    if (zip_open(&zip, raw, raw_len, err, sizeof err) == 0) {
        entry = zip_find_prefix(&zip, SAVE_ENTRY_PREFIX);
        CHECK(entry != NULL, "it holds a savefile");
        if (entry) {
            size_t save_len = entry->uncomp_size;

            save = zip_read(&zip, entry, err, sizeof err);
            CHECK(save != NULL, "which unpacks and passes its checksum (%s)", err);
            CHECK(save_len > 0, "and is not empty (%lu bytes)", (unsigned long)save_len);

            /* It is shipped gzipped, so unwrap it as an old build would be
             * handed it: a real savefile, through a real encoder's stream. */
            if (save && gz_is_gzip(save, save_len)) {
                size_t plain_len = 0;
                unsigned char *plain = gz_extract(save, save_len, &plain_len, err, sizeof err);

                CHECK(plain != NULL, "it unwraps out of gzip (%s)", err);
                CHECK(plain_len > save_len, "to a whole savefile (%lu bytes)",
                      (unsigned long)plain_len);
                if (plain) {
                    free(save);
                    save = plain;
                    save_len = plain_len;
                }
            }

            /* And back into gzip with our own compressor: a real savefile is
             * the input that matters, and the game's own copy of it is the
             * yardstick for what the result should cost. */
            if (save) {
                char err2[TB_ERR_LEN];
                unsigned char *packed, *back;
                size_t packed_len = 0, back_len = 0;

                packed = gz_compress(save, save_len, SAVE_NAME, &packed_len);
                CHECK(packed_len < save_len / 100,
                      "it compresses to under a hundredth (%lu bytes)",
                      (unsigned long)packed_len);
                back = gz_extract(packed, packed_len, &back_len, err2, sizeof err2);
                CHECK(back != NULL, "and unwraps again (%s)", err2);
                CHECK(back && back_len == save_len && memcmp(back, save, save_len) == 0,
                      "to the same savefile, byte for byte");
                free(back);
                free(packed);
            }

            /*
             * A whole savefile through the same write-and-read-back the swap
             * uses. A real one is tens of megabytes, which is exactly the size
             * at which a quietly truncated read would go unnoticed.
             */
            if (save) {
                char *scratch = test_dir("save_shipped");
                char *file = path_join(scratch, SAVE_NAME);
                unsigned char *back;
                size_t back_len = 0;

                CHECK(test_write_bytes(file, save, save_len) == 0, "it writes out whole");
                back = test_read_bytes(file, &back_len);
                CHECK_NUM(back_len, save_len, "and reads back at full length");
                CHECK(back && memcmp(back, save, save_len) == 0, "byte for byte");
                free(back);
                free(file);
                free(scratch);
            }
            free(save);
        }
        zip_close(&zip);
    } else {
        CHECK(0, "it is a valid archive: %s", err);
    }
}

/* ---- Steam Cloud ------------------------------------------------------- */

/*
 * Steam keeps its own copy of the savefile per account and prefers it to the
 * local one, so tabber has to deal with every account that has N++ data. The
 * fixtures below build a Steam folder with several accounts in it.
 */
#define CLOUD_OLD_SAVE  "the copy Steam is holding on to"

/* <steam>/userdata/<id>/230270/remote, created. Caller frees. */
static char *cloud_dir(const char *steam, const char *id, int with_app)
{
    char *user = path_join(steam, CLOUD_USERDATA_DIR);
    char *account = path_join(user, id);
    char *app = with_app ? path_join(account, NPP_STEAM_APPID) : str_dup(account);
    char *remote = with_app ? path_join(app, CLOUD_REMOTE_DIR) : str_dup(app);

    plat_mkdir_p(remote);
    free(user);
    free(account);
    free(app);
    return remote;
}

/* Puts a cloud save named `name` holding `data` in an account's folder. */
static void cloud_put(const char *remote, const char *name, const void *data, size_t len)
{
    char *path = path_join(remote, name);

    test_write_bytes(path, data, len);
    free(path);
}

static int cloud_has(const char *remote, const char *name)
{
    char *path = path_join(remote, name);
    int there = plat_is_file(path);

    free(path);
    return there;
}

/* The bytes of a cloud save. Caller frees. */
static unsigned char *cloud_read(const char *remote, const char *name, size_t *len_out)
{
    char *path = path_join(remote, name);
    unsigned char *data = test_read_bytes(path, len_out);

    free(path);
    return data;
}

/* Finds an account's line in a report. */
static const cloud_user *cloud_find(const cloud_report *report, const char *id)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        if (strcmp(report->users[i].id, id) == 0)
            return &report->users[i];
    }
    return NULL;
}

/*
 * Every account with N++ data is found, and only those: an account that has
 * other games but not this one, and a folder that is not an account at all,
 * are passed over.
 */
static void test_cloud_accounts(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("cloud_find");
    char *steam = path_join(root, "Steam");
    npp_paths paths = {0};
    cloud_report report;
    char *with_npp, *other_game, *not_an_account;

    test_case("finding the accounts with N++ cloud data");
    with_npp = cloud_dir(steam, "76561198000000001", 1);
    other_game = cloud_dir(steam, "76561198000000002", 0);   /* no N++ folder */
    not_an_account = cloud_dir(steam, "config", 1);          /* not an ID     */
    paths.steam_dir = steam;

    CHECK(cloud_apply(&paths, CLOUD_REMOVE, NULL, 0, &report, err, sizeof err) == 0,
          "the sweep runs (%s)", err);
    CHECK(report.searched, "Steam's folder was looked at");
    CHECK_NUM(report.count, 1, "only the account with N++ data is listed");
    CHECK(cloud_find(&report, "76561198000000001") != NULL, "and it is the right one");
    CHECK_NUM(report.touched, 0, "it has no cloud save, so nothing was touched");
    cloud_report_free(&report);

    /* No Steam folder at all: nothing to do, and not a failure. */
    {
        npp_paths nowhere = {0};

        CHECK(cloud_apply(&nowhere, CLOUD_REMOVE, NULL, 0, &report, err, sizeof err) == 0,
              "a game found without Steam is no obstacle");
        CHECK(!report.searched, "and nothing was searched");
        CHECK_NUM(report.count, 0, "so no account is listed");
        cloud_report_free(&report);
    }

    free(with_npp);
    free(other_game);
    free(not_an_account);
    free(steam);
    free(root);
}

/* Replacing: the cloud copy ends up holding the save that just went in. */
static void test_cloud_replace(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("cloud_replace");
    char *steam = path_join(root, "Steam");
    npp_paths paths = {0};
    cloud_report report;
    const cloud_user *user;
    unsigned char *gz, *written, *plain;
    size_t gz_len = 0, written_len = 0, plain_len = 0;
    char *remote;

    test_case("replacing the cloud save");
    remote = cloud_dir(steam, "111", 1);
    paths.steam_dir = steam;
    gz = test_gzip(CLOUD_OLD_SAVE, strlen(CLOUD_OLD_SAVE), &gz_len);
    cloud_put(remote, SAVE_GZ_NAME, gz, gz_len);

    /* The save going in is uncompressed, but the cloud only takes gzip: the
     * quota is 3 MB and the uncompressed savefile is seventy times that. */
    CHECK(cloud_apply(&paths, CLOUD_REPLACE, (const unsigned char *)TAB_SAVE,
                      strlen(TAB_SAVE), &report, err, sizeof err) == 0,
          "the sweep runs (%s)", err);
    user = cloud_find(&report, "111");
    CHECK(user && user->replaced, "the cloud save was replaced");
    CHECK(user && user->written > 0, "and its size reported");

    written = cloud_read(remote, SAVE_GZ_NAME, &written_len);
    CHECK(written && gz_is_gzip(written, written_len), "what landed is gzipped");
    plain = written ? gz_extract(written, written_len, &plain_len, err, sizeof err) : NULL;
    CHECK(plain != NULL, "and unwraps (%s)", err);
    CHECK(plain && plain_len == strlen(TAB_SAVE) && memcmp(plain, TAB_SAVE, plain_len) == 0,
          "to the save that went into the game");
    free(plain);
    free(written);
    cloud_report_free(&report);

    /* A save that is already gzipped goes up as it is. */
    CHECK(cloud_apply(&paths, CLOUD_REPLACE, gz, gz_len, &report, err, sizeof err) == 0,
          "a gzipped save needs no work");
    written = cloud_read(remote, SAVE_GZ_NAME, &written_len);
    CHECK(written && written_len == gz_len && memcmp(written, gz, gz_len) == 0,
          "it was copied up byte for byte");
    free(written);
    cloud_report_free(&report);

    free(gz);
    free(remote);
    free(steam);
    free(root);
}

/* Removing, keeping, and the uncompressed copy that always goes. */
static void test_cloud_modes(void)
{
    char err[TB_ERR_LEN];
    char *root = test_dir("cloud_modes");
    char *steam = path_join(root, "Steam");
    npp_paths paths = {0};
    cloud_report report;
    const cloud_user *user;
    unsigned char *gz;
    size_t gz_len = 0;
    char *remote;

    test_case("the cloud modes");
    remote = cloud_dir(steam, "222", 1);
    paths.steam_dir = steam;
    gz = test_gzip(CLOUD_OLD_SAVE, strlen(CLOUD_OLD_SAVE), &gz_len);

    /* keep: found, reported, untouched. */
    cloud_put(remote, SAVE_GZ_NAME, gz, gz_len);
    CHECK(cloud_apply(&paths, CLOUD_KEEP, (const unsigned char *)TAB_SAVE,
                      strlen(TAB_SAVE), &report, err, sizeof err) == 0, "keep runs");
    user = cloud_find(&report, "222");
    CHECK(user && user->had_gz, "the cloud save was found");
    CHECK(user && !user->replaced && !user->removed_gz, "and left exactly as it was");
    CHECK(cloud_has(remote, SAVE_GZ_NAME), "so it is still there");
    cloud_report_free(&report);

    /* remove: it goes. */
    CHECK(cloud_apply(&paths, CLOUD_REMOVE, (const unsigned char *)TAB_SAVE,
                      strlen(TAB_SAVE), &report, err, sizeof err) == 0, "remove runs");
    user = cloud_find(&report, "222");
    CHECK(user && user->removed_gz, "the cloud save was removed");
    CHECK(!cloud_has(remote, SAVE_GZ_NAME), "and is gone");
    cloud_report_free(&report);

    /*
     * An uncompressed cloud save can only be a leftover from before Steam
     * Cloud was switched off, in a format nothing reads any more: it goes
     * whatever the mode, and nothing is put in its place.
     */
    cloud_put(remote, SAVE_NAME, "an ancient cloud save", 21);
    CHECK(cloud_apply(&paths, CLOUD_REPLACE, (const unsigned char *)TAB_SAVE,
                      strlen(TAB_SAVE), &report, err, sizeof err) == 0, "replace runs");
    user = cloud_find(&report, "222");
    CHECK(user && user->removed_raw, "the uncompressed one was removed");
    CHECK(user && !user->replaced, "and nothing was uploaded in its place");
    CHECK(!cloud_has(remote, SAVE_NAME), "it is gone");
    CHECK(!cloud_has(remote, SAVE_GZ_NAME), "and no gzipped one was invented");
    cloud_report_free(&report);

    /* Both at once: the old one goes, the gzipped one is replaced. */
    cloud_put(remote, SAVE_NAME, "an ancient cloud save", 21);
    cloud_put(remote, SAVE_GZ_NAME, gz, gz_len);
    CHECK(cloud_apply(&paths, CLOUD_REPLACE, (const unsigned char *)TAB_SAVE,
                      strlen(TAB_SAVE), &report, err, sizeof err) == 0, "both are handled");
    user = cloud_find(&report, "222");
    CHECK(user && user->removed_raw && user->replaced, "one removed, one replaced");
    CHECK(!cloud_has(remote, SAVE_NAME), "the uncompressed one is gone");
    CHECK(cloud_has(remote, SAVE_GZ_NAME), "the gzipped one is there");
    cloud_report_free(&report);

    /* An account with no cloud save is reported, and nothing is written. */
    {
        char *empty = cloud_dir(steam, "333", 1);

        CHECK(cloud_apply(&paths, CLOUD_REPLACE, (const unsigned char *)TAB_SAVE,
                          strlen(TAB_SAVE), &report, err, sizeof err) == 0,
              "the sweep runs");
        user = cloud_find(&report, "333");
        CHECK(user && !user->had_gz && !user->had_raw, "the account is listed as empty");
        CHECK(!cloud_has(empty, SAVE_GZ_NAME), "and nothing was put there");
        CHECK_NUM(report.count, 2, "both accounts are in the report");
        cloud_report_free(&report);
        free(empty);
    }

    /* Modes are named on the command line, so they have to parse. */
    {
        cloud_mode mode = CLOUD_KEEP;

        CHECK(cloud_mode_parse("replace", &mode) == 0 && mode == CLOUD_REPLACE, "'replace'");
        CHECK(cloud_mode_parse("REMOVE", &mode) == 0 && mode == CLOUD_REMOVE, "'REMOVE'");
        CHECK(cloud_mode_parse("keep", &mode) == 0 && mode == CLOUD_KEEP, "'keep'");
        CHECK(cloud_mode_parse("nonsense", &mode) != 0, "and nothing else");
        CHECK(cloud_mode_parse(NULL, &mode) != 0, "not even nothing at all");
        CHECK_STR(cloud_mode_name(CLOUD_REPLACE), "replace", "and they have names");
    }

    free(gz);
    free(remote);
    free(steam);
    free(root);
}

void suite_save(void)
{
    test_suite("save");
    test_install_over_gzipped();
    test_gzipped_stays_gzipped();
    test_old_build_never_gets_gzip();
    test_both_forms_present();
    test_no_save_at_all();
    test_fresh_save_gzipped();
    test_force_compress();
    test_round_trip();
    test_uninstall_without_original();
    test_refusals();
    test_undo();
    test_old_installer_archives();
    test_shipped_save();
    test_cloud_accounts();
    test_cloud_replace();
    test_cloud_modes();
}
