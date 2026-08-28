#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "md5.h"
#include "net.h"
#include "platform.h"
#include "update.h"
#include "util.h"
#include "version.h"

/* ---- Versions ---------------------------------------------------------- */

/*
 * Reads one field of a version, leaving `p` on whatever followed it. A field
 * that is not there at all reads as zero, so "1.2" and "1.2.0" are one version
 * written two ways.
 */
static unsigned long version_field(const char **p)
{
    unsigned long value = 0;

    while (isdigit((unsigned char)**p)) {
        value = value * 10 + (unsigned long)(*(*p)++ - '0');
    }
    if (**p == '.')
        (*p)++;
    return value;
}

/* Whether what is left is a pre-release suffix rather than the end. */
static int has_suffix(const char *p)
{
    return *p != '\0';
}

int update_version_compare(const char *a, const char *b)
{
    int i;

    if (!a || !b)
        return a ? 1 : (b ? -1 : 0);
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;

    for (i = 0; i < 3; i++) {
        unsigned long fa = version_field(&a), fb = version_field(&b);

        if (fa != fb)
            return fa < fb ? -1 : 1;
    }

    /* Same numbers: "1.0-rc1" is the run-up to "1.0", so it sorts below it. */
    if (has_suffix(a) != has_suffix(b))
        return has_suffix(a) ? -1 : 1;
    return 0;
}

/* ---- The manifest ------------------------------------------------------ */

/*
 * The build for this platform and front-end: the exact key first, then the
 * bare system, for a release that ships one build per system. The front-end's
 * suffix is on both, so a GUI never falls back onto a CLI build. `key` comes
 * back holding the exact one, which is what a message about a release that
 * has nothing for us should name.
 */
static const json_value *build_for_us(const json_value *builds,
                                      const char *flavour,
                                      char *key, size_t keysz)
{
    char any[UPDATE_KEY_MAX];
    const json_value *build;

    snprintf(key, keysz, "%s%s", UPDATE_BUILD_KEY, flavour);
    build = json_get(builds, key);
    if (build)
        return build;
    snprintf(any, sizeof any, "%s%s", UPDATE_OS, flavour);
    return json_get(builds, any);
}

int update_manifest_parse(const char *text, const char *flavour,
                          update_info *info, char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    json_value *root;
    const json_value *build;
    const char *version, *md5;

    memset(info, 0, sizeof(*info));

    root = json_parse(text, sub, sizeof sub);
    if (!root) {
        err_set(err, errsz, "the release manifest is not valid JSON (%s)", sub);
        return -1;
    }

    version = json_get_string(root, UJK_VERSION, NULL);
    if (!version || !version[0]) {
        err_set(err, errsz, "the release manifest names no '%s'", UJK_VERSION);
        json_free(root);
        return -1;
    }
    snprintf(info->version, sizeof info->version, "%s", version);
    snprintf(info->date, sizeof info->date, "%s",
             json_get_string(root, UJK_DATE, ""));
    info->notes = str_dup(json_get_string(root, UJK_NOTES, NULL));
    info->page = str_dup(json_get_string(root, UJK_PAGE, UPDATE_RELEASES_URL));
    info->newer = update_version_compare(info->version, TABBER_VERSION) > 0;

    /*
     * A release with no build for this platform is still worth reporting —
     * knowing a newer version exists is the point — so a missing build leaves
     * `url` NULL rather than failing.
     */
    build = build_for_us(json_get(root, UJK_BUILDS), flavour,
                         info->build, sizeof info->build);
    if (build) {
        info->url = str_dup(json_get_string(build, UJK_URL, NULL));
        info->size = (size_t)json_get_int(build, UJK_SIZE, 0);
        md5 = json_get_string(build, UJK_MD5, "");
        snprintf(info->md5, sizeof info->md5, "%s", md5);
        if (info->url && (info->size == 0 || strlen(info->md5) != MD5_HEX_LEN)) {
            /* Half a description is worse than none: without both we cannot
             * tell a good download from a bad one, so we will not take it.
             * The message is built before `info` is emptied below. */
            err_set(err, errsz, "the release manifest describes the %s build without a "
                                "usable '%s' and '%s'", info->build, UJK_SIZE, UJK_MD5);
            json_free(root);
            update_info_free(info);
            return -1;
        }
    }

    json_free(root);
    return 0;
}

int update_check(const char *flavour, update_info *info, char *err, size_t errsz)
{
    char *text = NULL;
    size_t len = 0;
    int rc;

    memset(info, 0, sizeof(*info));
    if (net_fetch(UPDATE_MANIFEST_URL, &text, &len, err, errsz) != 0)
        return -1;

    rc = update_manifest_parse(text, flavour, info, err, errsz);
    free(text);
    return rc;
}

void update_info_free(update_info *info)
{
    free(info->notes);
    free(info->page);
    free(info->url);
    memset(info, 0, sizeof(*info));
}

/* ---- Staging ----------------------------------------------------------- */

/* The binary to replace: this one, unless a test has named a stand-in. */
static char *running_exe(void)
{
    char *override = plat_getenv(UPDATE_ENV_EXE);

    return override ? override : plat_exe_path();
}

/* Fills in the three names an update works with: the running binary, the one
 * being written beside it, and where the old one goes. */
static int plan_paths(update_plan *plan, char *err, size_t errsz)
{
    plan->exe = running_exe();
    if (!plan->exe) {
        err_set(err, errsz, "tabber cannot tell where its own executable is, so it "
                            "cannot replace it");
        return -1;
    }
    plan->staged = str_fmt("%s%s", plan->exe, UPDATE_NEW_SUFFIX);
    plan->aside = str_fmt("%s%s", plan->exe, UPDATE_OLD_SUFFIX);
    return 0;
}

int update_plan_stage(const update_info *info, const void *data, size_t len,
                      update_plan *plan, char *err, size_t errsz)
{
    char digest[MD5_HEX_LEN + 1];

    memset(plan, 0, sizeof(*plan));
    if (plan_paths(plan, err, errsz) != 0)
        goto fail;

    snprintf(plan->version, sizeof plan->version, "%s", info->version);
    plan->bytes = len;

    /* Both of the manifest's promises, before a byte of it reaches the disk. */
    if (len != info->size) {
        err_set(err, errsz, "the download is %lu bytes, and the manifest promised %lu",
                (unsigned long)len, (unsigned long)info->size);
        goto fail;
    }
    md5_hex(data, len, digest);
    if (!str_ieq(digest, info->md5)) {
        err_set(err, errsz, "the download's MD5 is %s, and the manifest promised %s",
                digest, info->md5);
        goto fail;
    }

    /*
     * Beside the running binary on purpose: the swap that follows is a rename,
     * and a rename is only atomic — on Windows, only possible at all — within
     * one filesystem.
     */
    if (plat_write_file(plan->staged, data, len) != 0) {
        err_set(err, errsz, "cannot write '%s'; is the folder tabber lives in "
                            "writable?", plan->staged);
        goto fail;
    }
    if (plat_make_executable(plan->staged) != 0) {
        err_set(err, errsz, "'%s' cannot be marked executable", plan->staged);
        plat_remove_file(plan->staged);
        goto fail;
    }
    return 0;

fail:
    update_plan_free(plan);
    return -1;
}

int update_plan_build(const update_info *info, update_plan *plan, char *err, size_t errsz)
{
    char *data = NULL;
    size_t len = 0;
    int rc;

    memset(plan, 0, sizeof(*plan));
    if (!info->url) {
        err_set(err, errsz, "this release ships no build for %s", info->build);
        return -1;
    }
    if (net_fetch(info->url, &data, &len, err, errsz) != 0)
        return -1;

    rc = update_plan_stage(info, data, len, plan, err, errsz);
    free(data);
    return rc;
}

/* ---- Applying ---------------------------------------------------------- */

/*
 * Asks the new binary what it is. Anything other than a clean exit from the
 * version we were promised means we do not keep it — a file that was verified
 * byte for byte can still be a build that will not run here.
 */
static int self_check(const update_plan *plan)
{
    char *args[2];
    int status = -1;

    args[0] = (char *)UPDATE_SELF_CHECK_ARG;
    args[1] = (char *)plan->version;
    if (plat_run_and_wait(plan->exe, args, 2, &status) != 0)
        return -1;
    return status == 0 ? 0 : -1;
}

int update_plan_apply(update_plan *plan, char *err, size_t errsz)
{
    /*
     * A running executable cannot be deleted or written to, but it can be
     * renamed: the process keeps running from the file under its new name,
     * which leaves the old name free for the new binary.
     */
    if (plat_replace_file(plan->exe, plan->aside) != 0) {
        err_set(err, errsz, "cannot move '%s' aside; check the folder's permissions",
                plan->exe);
        return -1;
    }
    if (plat_replace_file(plan->staged, plan->exe) != 0) {
        err_set(err, errsz, "cannot put the new tabber in place at '%s'", plan->exe);
        plat_replace_file(plan->aside, plan->exe);      /* straight back */
        return -1;
    }
    plan->applied = 1;

    if (self_check(plan) != 0) {
        err_set(err, errsz, "the new tabber did not start, or is not the %s it was "
                            "said to be; the one you had is back in place",
                plan->version);
        update_plan_undo(plan);
        return -1;
    }
    return 0;
}

void update_plan_undo(update_plan *plan)
{
    if (!plan->applied)
        return;
    plat_remove_file(plan->exe);                 /* the new one, unwanted now */
    plat_replace_file(plan->aside, plan->exe);
    plan->applied = 0;
}

void update_plan_free(update_plan *plan)
{
    free(plan->exe);
    free(plan->staged);
    free(plan->aside);
    memset(plan, 0, sizeof(*plan));
}

void update_sweep(void)
{
    char *exe = running_exe();
    char *aside, *staged;

    if (!exe)
        return;
    aside = str_fmt("%s%s", exe, UPDATE_OLD_SUFFIX);
    staged = str_fmt("%s%s", exe, UPDATE_NEW_SUFFIX);

    /* Both are leftovers by definition: the old binary is only unlocked once
     * the process that was running it has gone, and a staged one that is still
     * here belongs to an update that never finished. */
    if (plat_is_file(aside))
        plat_remove_file(aside);
    if (plat_is_file(staged))
        plat_remove_file(staged);

    free(staged);
    free(aside);
    free(exe);
}
