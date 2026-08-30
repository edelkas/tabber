/*
 * log.h - The tool's running account of what it has done.
 *
 * Everything the tool has to say about a step it took goes through log_line:
 * a fetch that started, an install that finished, a look that found nothing.
 * The line is written out where there is anywhere to write it — a console, or
 * whatever the output was redirected to — and kept in memory either way, so a
 * front-end without a console has something to show. The graphical one puts
 * the newest of them along the bottom of its window.
 *
 * A logged line is one line: what is handed in is folded onto a single one,
 * runs of whitespace and all, and cut short with an ellipsis if it runs past
 * LOG_LINE_MAX. That is what lets one call serve both a paragraph shown in a
 * dialog and a line shown in a status bar.
 *
 * What this is not is the tool's error reporting. What goes wrong is still
 * handed back to the caller in its own `err` buffer and reported by whoever
 * asked for it; the log is the account of what happened, not a way of saying
 * that it did not.
 *
 * The lines can also be kept: switched on with log_set_saving, every one of
 * them is appended to LOG_FILENAME in the tool's own folder, dated as well as
 * timed because that file outlives the run that wrote it. Both front-ends
 * write to it, so it is the account of everything the tool has done to this
 * machine and not of one window's sitting. It is capped, and the one before it
 * is kept beside it; see log_set_saving.
 *
 * Not thread-safe: the tool is single-threaded, front-ends included.
 */
#ifndef TABBER_LOG_H
#define TABBER_LOG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest line kept, terminator included. Anything longer is cut with an
 * ellipsis: this is a line of news, not a place to keep a document. */
#define LOG_LINE_MAX 512

/* How many are kept. The oldest falls off the end when a new one arrives. */
#define LOG_HISTORY  128

/* What is cut short ends with this, so a reader can tell that it was. */
#define LOG_ELLIPSIS "..."

/* The file the lines are kept in, in the tool's own folder, and what the one
 * before it is called once the file has grown past LOG_FILE_MAX bytes. */
#define LOG_FILENAME   "tabber.log"
#define LOG_OLD_SUFFIX ".old"
#define LOG_FILE_MAX   (1024L * 1024L)

typedef struct {
    long long when;          /* Unix time, as the rest of the tool counts it */
    char text[LOG_LINE_MAX];
} log_entry;

/* Records a line, and writes it out where there is anywhere to write it. The
 * format takes no trailing newline: a line is one by construction. */
void log_line(const char *fmt, ...);

/* The same, for a caller that already has the arguments in hand. */
void log_linev(const char *fmt, va_list ap);

/* The newest line, or NULL when nothing has been logged yet. */
const log_entry *log_last(void);

/* How many are being kept, which stops climbing at LOG_HISTORY. */
size_t log_count(void);

/* One of them by age: 0 is the newest, log_count() - 1 the oldest kept. NULL
 * when there is no such line. */
const log_entry *log_at(size_t age);

/* Forgets the lot. Mainly for tests. */
void log_clear(void);

/*
 * Whether lines are also written out as they are logged. On by default; the
 * test suite turns it off so that the tool's own account does not land in the
 * middle of the suite's.
 */
void log_set_echo(int on);

/*
 * Whether they are also appended to the logfile. Off until it is asked for,
 * which both front-ends do as soon as they know they are wanted to: the CLI
 * once it has read its arguments, unless --no-logfile was among them, and the
 * window once it has read its settings. Off by default rather than on because
 * a run that is only being asked a question — a self-check, a version — should
 * leave nothing behind, and because it is what makes the two switches the only
 * thing that decides it.
 *
 * Once the file passes LOG_FILE_MAX it is set aside under LOG_OLD_SUFFIX,
 * replacing whatever was there, and the next line starts a fresh one. So two
 * files' worth is the most that is ever kept, and the tool's folder cannot
 * quietly fill up with an account of it.
 */
void log_set_saving(int on);

/* Where that file is, or NULL when the tool's folder cannot be found. The
 * caller frees it. */
char *log_file_path(void);

#ifdef __cplusplus
}
#endif

#endif /* TABBER_LOG_H */
