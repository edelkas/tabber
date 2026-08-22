#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "palettes.h"
#include "platform.h"
#include "tabs.h"
#include "util.h"

/* ---- The baked palettes ------------------------------------------------ */

/*
 * Every palette baked into the library, in load order, which is also the order
 * their IDs run in. The four cut before release were baked with " CUT"
 * appended (IDs 16, 38, 94 and 115), so their plain names are free to use and
 * a custom palette can bring them back.
 *
 * The six palettes added in TEN++ are not here: those ship as ordinary folders
 * in the game's palettes folder, where they are found by looking.
 */
const char *const palette_baked_names[PALETTE_BAKED_COUNT] = {
    "BASIC", "F7200", "acid",                                      /*   0 */
    "airline", "birthday cake", "blueprint",                       /*   3 */
    "bordeaux", "chemical", "chococherry",                         /*   6 */
    "classic", "clean", "console",                                 /*   9 */
    "disassembly", "dorado", "dusk",                               /*  12 */
    "epaper", "epaper invert CUT", "evening",                      /*  15 */
    "galactic", "gothmode", "holosphere",                          /*  18 */
    "hot", "infographic", "invert",                                /*  21 */
    "kicks", "lightcycle", "m",                                    /*  24 */
    "metoro", "midnight", "minus",                                 /*  27 */
    "mir", "mono", "moonbase",                                     /*  30 */
    "neptune", "oceanographer", "okinami",                         /*  33 */
    "orbit", "pale", "papier CUT",                                 /*  36 */
    "papier invert", "party", "pinku",                             /*  39 */
    "plus", "poseidon", "pulse",                                   /*  42 */
    "quench", "replicant", "retro",                                /*  45 */
    "shift", "shock", "simulator",                                 /*  48 */
    "solarized dark", "solarized light", "supernavy",              /*  51 */
    "toxin", "vasquez", "virtual",                                 /*  54 */
    "vivid", "wizard", "yeti",                                     /*  57 */
    "pumpkin", "witchy", "argon",                                  /*  60 */
    "autumn", "berry", "bloodmoon",                                /*  63 */
    "brink", "cacao", "champagne",                                 /*  66 */
    "concrete", "cowboy", "dagobah",                               /*  69 */
    "debugger", "delicate", "desert world",                        /*  72 */
    "elephant", "florist", "formal",                               /*  75 */
    "gatecrasher", "grapefrukt", "grappa",                         /*  78 */
    "gunmetal", "hazard", "heirloom",                              /*  81 */
    "hope", "hyperspace", "ice world",                             /*  84 */
    "incorporated", "jaune", "juicy",                              /*  87 */
    "lab", "lava world", "lemonade",                               /*  90 */
    "lichen", "line CUT", "machine",                               /*  93 */
    "mustard", "mute", "nemk",                                     /*  96 */
    "neutrality", "noctis", "petal",                               /*  99 */
    "PICO-8", "porphyrous", "QDUST",                               /* 102 */
    "regal", "rust", "sakura",                                     /* 105 */
    "sinister", "starfighter", "sunset",                           /* 108 */
    "synergy", "talisman", "toothpaste",                           /* 111 */
    "TR-808", "tycho CUT", "vectrex",                              /* 114 */
    "vintage", "void", "waka",                                     /* 117 */
    "wyvern", "xenon", "powder",                                   /* 120 */
};

int palette_is_baked(const char *name)
{
    size_t i;

    for (i = 0; i < PALETTE_BAKED_COUNT; i++) {
        if (str_ieq(palette_baked_names[i], name))
            return 1;
    }
    return 0;
}

/* ---- Modes and outcomes ------------------------------------------------ */

int palette_collision_parse(const char *text, palette_collision *out)
{
    if (!text)
        return -1;
    if (str_ieq(text, "skip"))    { *out = PALETTE_SKIP;    return 0; }
    if (str_ieq(text, "replace")) { *out = PALETTE_REPLACE; return 0; }
    if (str_ieq(text, "suffix"))  { *out = PALETTE_SUFFIX;  return 0; }
    return -1;
}

const char *palette_collision_name(palette_collision mode)
{
    switch (mode) {
        case PALETTE_SKIP:    return "skip";
        case PALETTE_REPLACE: return "replace";
        case PALETTE_SUFFIX:  return "suffix";
        default:              return "?";
    }
}

const char *palette_outcome_text(palette_outcome outcome)
{
    switch (outcome) {
        case PAL_INSTALLED:       return "installed";
        case PAL_RENAMED:         return "installed under another name";
        case PAL_REPLACED:        return "replaced the one that was there";
        case PAL_SKIPPED_PRESENT: return "skipped, the game already has that palette";
        case PAL_SKIPPED_BAKED:   return "skipped, the game bakes a palette of that name";
        case PAL_SKIPPED_DUP:     return "skipped, the tab bundles it twice";
        case PAL_SKIPPED_FULL:    return "skipped, the game's palette limit is full";
        case PAL_REMOVED:         return "removed";
        case PAL_ABSENT:          return "was not there any more";
        case PAL_KEPT:            return "left in place";
        default:                  return "failed";
    }
}

/* ---- Plan -------------------------------------------------------------- */

/* One bundled palette on its way into the game's folder. */
struct palette_entry {
    char *name;                /* as the tab names it                        */
    char *source;              /* its folder in the tab store                */
    char *target;              /* name it takes in the game, NULL when none  */
    char *dest;                /* the folder to create, NULL when none       */
    char *stash;               /* where the palette it replaces is kept      */
    palette_outcome outcome;
    size_t files;              /* files copied in                            */
    int copied;                /* the folder is on disk                      */
    int stashed;               /* the old palette was moved aside            */
    char detail[TB_ERR_LEN];
};

void palettes_bundled(const npp_tab *tab, str_list *out)
{
    const json_value *list = json_get(json_get(tab->node, TJK_DISK), PJK_PALETTES);
    const json_value *item;

    if (!list || list->type != JSON_ARRAY)
        return;
    for (item = list->children; item; item = item->next) {
        if (item->type == JSON_STRING && item->string[0])
            str_list_push(out, str_dup(item->string));
    }
}

/* The game's palettes folder, which need not exist yet. Caller frees. */
static char *game_palettes_dir(const digest *dig, const npp_paths *paths)
{
    char *assets = path_join(paths->install_dir, NPP_ASSETS_SUBDIR);
    char *dir = path_join(assets, digest_palettes_dir(dig));

    free(assets);
    return dir;
}

/*
 * Names of the palettes already in `dir`, one per subfolder, and how many of
 * them the game actually counts. Every name goes into `out`, since every name
 * is taken as far as a new palette is concerned, but a folder named after a
 * baked palette is dropped before the game even tries to parse it and so takes
 * up none of the 256 slots. Keeping copies of the baked palettes around is
 * common — the Linux build ships them, and they are handy to edit from — and
 * counting those would tally them twice.
 *
 * A folder that is not there yet simply has none.
 */
static size_t list_existing(const char *dir, str_list *out)
{
    str_list entries = {0};
    size_t i, count = 0;

    if (plat_list_dir(dir, &entries) != 0)
        return 0;
    for (i = 0; i < entries.count; i++) {
        char *sub = path_join(dir, entries.items[i]);

        if (plat_is_dir(sub)) {
            str_list_push(out, str_dup(entries.items[i]));
            if (!palette_is_baked(entries.items[i]))
                count++;
        }
        free(sub);
    }
    str_list_free(&entries);
    return count;
}

/* The first of "<base> 2", "<base> 3", ... that nothing else answers to. */
static char *free_name(const char *base, const str_list *taken)
{
    int n;

    for (n = 2; n <= PALETTE_RENAME_MAX; n++) {
        char *candidate = str_fmt(PALETTE_RENAME_FMT, base, n);

        if (!palette_is_baked(candidate) && !str_list_contains(taken, candidate))
            return candidate;
        free(candidate);
    }
    return NULL;
}

/* Whether an earlier palette of this tab already answers to `name`. */
static int bundled_twice(const palette_plan *plan, size_t upto, const char *name)
{
    size_t i;

    for (i = 0; i < upto; i++) {
        if (str_ieq(plan->items[i].name, name))
            return 1;
    }
    return 0;
}

/*
 * Settles what happens to one palette: the name it takes, or why it takes
 * none. `taken` holds every name already spoken for, in the game's folder and
 * by earlier palettes of this same tab.
 */
static void resolve_target(palette_plan *plan, size_t index, str_list *taken)
{
    palette_entry *item = &plan->items[index];
    int baked = palette_is_baked(item->name);
    int present = str_list_contains(taken, item->name);
    char *target = NULL;

    if (bundled_twice(plan, index, item->name)) {
        item->outcome = PAL_SKIPPED_DUP;
        return;
    }

    if (!baked && !present) {
        target = str_dup(item->name);
        item->outcome = PAL_INSTALLED;
    } else if (plan->mode == PALETTE_SUFFIX) {
        target = free_name(item->name, taken);
        if (!target) {
            item->outcome = PAL_SKIPPED_PRESENT;
            snprintf(item->detail, sizeof item->detail,
                     "every name up to '" PALETTE_RENAME_FMT "' is taken as well",
                     item->name, PALETTE_RENAME_MAX);
            return;
        }
        item->outcome = PAL_RENAMED;
    } else if (baked) {
        /* Nothing on disk can win against the library: the game drops the
         * folder before reading it, so the palette would simply never appear.
         * It costs nothing to leave it out, and nothing to put it in either. */
        item->outcome = PAL_SKIPPED_BAKED;
        return;
    } else if (plan->mode == PALETTE_REPLACE) {
        target = str_dup(item->name);
        item->outcome = PAL_REPLACED;
    } else {
        item->outcome = PAL_SKIPPED_PRESENT;
        return;
    }

    /*
     * Room, checked last: a palette that replaces one, or is skipped because
     * its name is taken, adds nothing to the count and so can never be the one
     * that does not fit.
     */
    if (item->outcome != PAL_REPLACED &&
        PALETTE_BAKED_COUNT + plan->existing + plan->adding >= PALETTE_LIMIT) {
        item->outcome = PAL_SKIPPED_FULL;
        snprintf(item->detail, sizeof item->detail,
                 "the game parses %d palettes and already has %u",
                 PALETTE_LIMIT, (unsigned)(PALETTE_BAKED_COUNT + plan->existing + plan->adding));
        free(target);
        return;
    }

    if (item->outcome != PAL_REPLACED)
        plan->adding++;
    item->target = target;
    item->dest = path_join(plan->dir, target);
    if (item->outcome == PAL_REPLACED)
        item->stash = str_fmt("%s%s", item->dest, PALETTE_STASH_SUFFIX);
    str_list_push(taken, str_dup(target));
}

int palettes_plan_build(const digest *dig, const npp_tab *tab, const npp_paths *paths,
                        palette_collision mode, palette_plan *plan,
                        char *err, size_t errsz)
{
    str_list names = {0}, taken = {0};
    size_t i;
    int rc = -1;

    memset(plan, 0, sizeof(*plan));
    plan->mode = mode;
    plan->dir = game_palettes_dir(dig, paths);

    palettes_bundled(tab, &names);
    if (names.count == 0) {          /* most tabs bundle none at all */
        rc = 0;
        goto done;
    }

    {
        char *tab_root = tab_dir_path(tab->code);
        plan->source_dir = path_join(tab_root, digest_palettes_dir(dig));
        free(tab_root);
    }
    if (!plat_is_dir(plan->source_dir)) {
        err_set(err, errsz, "the tab bundles %u palette(s) but '%s' is not in the tab "
                            "store; fetch the tab again",
                (unsigned)names.count, plan->source_dir);
        goto done;
    }

    plan->existing = list_existing(plan->dir, &taken);

    plan->items = xmalloc(names.count * sizeof(*plan->items));
    for (i = 0; i < names.count; i++) {
        palette_entry *item = &plan->items[plan->count++];

        memset(item, 0, sizeof(*item));
        item->name = str_dup(names.items[i]);
        item->source = path_join(plan->source_dir, item->name);
        if (!plat_is_dir(item->source)) {
            err_set(err, errsz, "the tab's palette '%s' is not in the tab store at '%s'; "
                                "fetch the tab again", item->name, item->source);
            goto done;
        }
        resolve_target(plan, plan->count - 1, &taken);
    }
    rc = 0;

done:
    str_list_free(&names);
    str_list_free(&taken);
    if (rc != 0)
        palettes_plan_free(plan);
    return rc;
}

/* ---- Applying ---------------------------------------------------------- */

/* Turns the settled plan into the per-palette lines the caller reports. */
static void fill_report(const palette_plan *plan, palette_report *report)
{
    size_t i;

    memset(report, 0, sizeof(*report));
    report->dir = str_dup(plan->dir);
    report->existing = plan->existing;
    report->count = plan->count;
    report->items = plan->count ? xmalloc(plan->count * sizeof(*report->items)) : NULL;

    for (i = 0; i < plan->count; i++) {
        const palette_entry *item = &plan->items[i];
        palette_item *line = &report->items[i];

        line->name = str_dup(item->name);
        line->target = str_dup(item->target);
        line->outcome = item->outcome;
        line->files = item->files;
        snprintf(line->detail, sizeof line->detail, "%s", item->detail);

        if (item->outcome == PAL_FAILED)
            report->failed++;
        else if (item->copied)
            report->installed++;
        else
            report->skipped++;
    }
    report->total = PALETTE_BAKED_COUNT + plan->existing + plan->adding;
}

int palettes_plan_apply(palette_plan *plan, palette_report *report,
                        char *err, size_t errsz)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        palette_entry *item = &plan->items[i];

        if (!item->dest)
            continue;                       /* nothing to do for this one */
        if (plat_mkdir_p(plan->dir) != 0) { /* the game may ship without it */
            err_set(err, errsz, "cannot create the game's palettes folder at '%s'",
                    plan->dir);
            goto failed;
        }

        /*
         * Move the palette being replaced aside rather than deleting it: until
         * the install is committed it may still have to be put back. It sits
         * in the palettes folder meanwhile, where the game would read it as
         * one more palette, but only until this install finishes.
         */
        if (item->stash && plat_is_dir(item->dest)) {
            if (plat_move_dir(item->dest, item->stash) != 0) {
                err_set(err, errsz, "cannot move '%s' aside to '%s'", item->dest,
                        item->stash);
                goto failed;
            }
            item->stashed = 1;
        }

        if (plat_copy_tree(item->source, item->dest, &item->files) != 0) {
            err_set(err, errsz, "cannot copy the palette '%s' into '%s'", item->name,
                    item->dest);
            goto failed;
        }
        item->copied = 1;
    }

    plan->applied = 1;
    fill_report(plan, report);
    return 0;

failed:
    plan->items[i].outcome = PAL_FAILED;
    snprintf(plan->items[i].detail, sizeof plan->items[i].detail, "%s", err ? err : "");
    palettes_plan_undo(plan);
    fill_report(plan, report);
    return -1;
}

void palettes_plan_undo(palette_plan *plan)
{
    size_t i = plan->count;

    while (i-- > 0) {
        palette_entry *item = &plan->items[i];

        if (item->dest && (item->copied || plat_is_dir(item->dest)))
            plat_remove_tree(item->dest);
        item->copied = 0;
        if (item->stashed) {
            plat_move_dir(item->stash, item->dest);
            item->stashed = 0;
        }
    }
    plan->applied = 0;
}

void palettes_plan_commit(palette_plan *plan)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        if (plan->items[i].stashed) {
            plat_remove_tree(plan->items[i].stash);
            plan->items[i].stashed = 0;
        }
    }
}

void palettes_plan_installed(const palette_plan *plan, str_list *out)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        if (plan->items[i].copied && plan->items[i].target)
            str_list_push(out, str_dup(plan->items[i].target));
    }
}

void palettes_plan_free(palette_plan *plan)
{
    size_t i;

    for (i = 0; i < plan->count; i++) {
        free(plan->items[i].name);
        free(plan->items[i].source);
        free(plan->items[i].target);
        free(plan->items[i].dest);
        free(plan->items[i].stash);
    }
    free(plan->items);
    free(plan->dir);
    free(plan->source_dir);
    memset(plan, 0, sizeof(*plan));
}

/* ---- Removing ---------------------------------------------------------- */

int palettes_remove(const digest *dig, const npp_paths *paths, const str_list *names,
                    int keep, palette_report *report, char *err, size_t errsz)
{
    str_list present = {0};
    size_t i;

    memset(report, 0, sizeof(*report));
    if (!paths->install_dir) {
        err_set(err, errsz, "the game's installation directory is not known");
        return -1;
    }
    report->dir = game_palettes_dir(dig, paths);
    report->existing = list_existing(report->dir, &present);
    str_list_free(&present);

    if (!names || names->count == 0) {
        report->total = PALETTE_BAKED_COUNT + report->existing;
        return 0;
    }

    report->count = names->count;
    report->items = xmalloc(names->count * sizeof(*report->items));
    for (i = 0; i < names->count; i++) {
        palette_item *line = &report->items[i];
        char *path = path_join(report->dir, names->items[i]);

        memset(line, 0, sizeof(*line));
        line->name = str_dup(names->items[i]);
        line->target = str_dup(names->items[i]);

        if (!plat_is_dir(path)) {
            line->outcome = PAL_ABSENT;       /* removed by hand, or never made */
            report->skipped++;
        } else if (keep) {
            line->outcome = PAL_KEPT;
            report->skipped++;
        } else if (plat_remove_tree(path) != 0) {
            line->outcome = PAL_FAILED;
            snprintf(line->detail, sizeof line->detail, "'%s' could not be deleted", path);
            report->failed++;
        } else {
            line->outcome = PAL_REMOVED;
            report->removed++;
        }
        free(path);
    }

    report->total = PALETTE_BAKED_COUNT + report->existing - report->removed;
    return 0;
}

void palette_report_free(palette_report *report)
{
    size_t i;

    for (i = 0; i < report->count; i++) {
        free(report->items[i].name);
        free(report->items[i].target);
    }
    free(report->items);
    free(report->dir);
    memset(report, 0, sizeof(*report));
}
