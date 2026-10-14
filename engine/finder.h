#ifndef FINDER_H
#define FINDER_H

/*
 * AmiFind - shared file search engine
 *
 * Recurses an AmigaDOS path (volume, assign or directory) and reports
 * every entry whose name matches a pattern.  Matching is either a
 * case-insensitive substring (plain text) or full AmigaDOS pattern
 * matching when the pattern contains wildcards (#? * ? [ ] ~ etc.).
 */

#include <exec/types.h>

struct Matcher {
    BOOL  useWild;        /* TRUE -> MatchPatternNoCase, FALSE -> substring */
    UBYTE parsed[512];    /* tokenised pattern for MatchPatternNoCase       */
    char  lower[208];     /* lower-cased pattern (>= GUI GTST_MaxChars 200) */
};

/* Called for every match. Return FALSE to abort the whole search. */
typedef BOOL (*FoundFunc)(CONST_STRPTR fullpath, BOOL isDir, LONG size, APTR user);

/* Called periodically. Return TRUE to abort the search (e.g. Ctrl-C / Stop). */
typedef BOOL (*PollFunc)(APTR user);

struct SearchStats {
    ULONG matches;
    ULONG dirsScanned;
    ULONG filesScanned;
    BOOL  aborted;
};

/* Prepare a matcher from a user-supplied pattern. Returns FALSE on a
 * malformed / over-long pattern. */
BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern);

/* TRUE if a single file/dir name matches the prepared matcher. */
BOOL matcherMatch(struct Matcher *m, CONST_STRPTR name);

/* Walk rootPath recursively.
 * Returns 0 on success, or a DOS error code (IoErr()) if the root could
 * not be locked. found/poll may be NULL. */
LONG searchTree(CONST_STRPTR rootPath, struct Matcher *m,
                FoundFunc found, APTR foundUser,
                PollFunc  poll,  APTR pollUser,
                struct SearchStats *stats);

#endif /* FINDER_H */
