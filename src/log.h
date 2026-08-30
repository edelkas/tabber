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

#ifdef __cplusplus
}
#endif

#endif /* TABBER_LOG_H */
