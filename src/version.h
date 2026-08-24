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
#define TABBER_VERSION  "0.1.0"
#endif

#endif /* TABBER_VERSION_H */
