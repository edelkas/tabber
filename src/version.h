/* version.h - Program identity, shared by the CLI and the HTTP user agent. */
#ifndef TABBER_VERSION_H
#define TABBER_VERSION_H

#define TABBER_NAME     "tabber"

/*
 * Overridable at build time (-DTABBER_VERSION='"0.9.0"'), which is how a
 * staging binary is built to rehearse an update against a release that does
 * not exist yet.
 */
#ifndef TABBER_VERSION
#define TABBER_VERSION  "0.4.0"
#endif

/* The day that version went out, shown in the front-end's About box. It moves
 * with TABBER_VERSION and is overridable the same way. */
#ifndef TABBER_DATE
#define TABBER_DATE     "2026-08-29"
#endif

#endif /* TABBER_VERSION_H */
