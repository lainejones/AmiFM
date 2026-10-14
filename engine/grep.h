#ifndef GREP_H
#define GREP_H

/*
 * AmiGrep - shared content-search engine
 *
 * Recurses an AmigaDOS path (volume, assign, directory or single file) and,
 * for every text file, reports each line that matches a pattern. Matching is
 * either a case-insensitive substring (plain text) or, when the pattern
 * contains AmigaDOS wildcards (#? * ? [] ~ |), a full AmigaDOS pattern test
 * applied across the whole line (the pattern is wrapped as #?<pat>#? so it
 * behaves like a "contains" match).
 *
 * An optional filename filter (itself an AmigaDOS wildcard, case-insensitive)
 * restricts which files are opened - e.g. only "#?.c".
 */

#include <exec/types.h>

#define GREP_MAXLINE 1024        /* longest line we accumulate/match          */

struct Matcher {
    BOOL  useWild;               /* TRUE -> Match[NoCase], FALSE -> substring  */
    BOOL  caseSensitive;
    UBYTE parsed[600];           /* tokenised "#?<pat>#?" for MatchPattern[..]  */
    char  needle[256];           /* literal needle (lower-cased if !case)       */
    int   needleLen;
};

/* optional filename filter (AmigaDOS wildcard, always case-insensitive) */
struct NameFilter {
    BOOL  active;
    UBYTE parsed[300];
};

/* Called for every matching line. line==0 / text=="" in names-only mode.
 * Return FALSE to abort the whole search. */
typedef BOOL (*MatchFunc)(CONST_STRPTR path, ULONG line, CONST_STRPTR text, APTR user);

/* Called periodically. Return TRUE to abort the search (Ctrl-C / Stop). */
typedef BOOL (*PollFunc)(APTR user);

struct GrepStats {
    ULONG dirsScanned;
    ULONG filesScanned;          /* text files actually searched               */
    ULONG filesMatched;          /* files with >= 1 matching line              */
    ULONG linesMatched;
    ULONG binarySkipped;         /* files skipped because they look binary     */
    BOOL  namesOnly;             /* IN: report only the first match per file   */
    BOOL  aborted;               /* OUT: search was stopped early              */
};

/* Prepare a matcher. Returns FALSE on a malformed / over-long pattern. */
BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern, BOOL caseSensitive);

/* Prepare a filename filter. pat NULL/empty -> inactive (match every file).
 * Returns FALSE on a malformed / over-long pattern. */
BOOL nameFilterInit(struct NameFilter *nf, CONST_STRPTR pat);
BOOL nameFilterMatch(struct NameFilter *nf, CONST_STRPTR name);

/* Walk rootPath recursively, grepping every (filtered) text file.
 * Returns 0 on success, or a DOS error code (IoErr()) if the root could not
 * be locked. found/poll may be NULL. Set stats->namesOnly before calling. */
LONG grepTree(CONST_STRPTR rootPath, struct Matcher *m, struct NameFilter *nf,
              MatchFunc found, APTR foundUser,
              PollFunc  poll,  APTR pollUser,
              struct GrepStats *stats);

#endif /* GREP_H */
