/*
 * AmiGrep - shared content-search engine (see grep.h)
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include "grep.h"

/* ------------------------------------------------------------------ */
/* small case-insensitive helpers                                     */
/* ------------------------------------------------------------------ */

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return c;
}

static void str_lower(char *dst, CONST_STRPTR src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = to_lower((char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

/* case-sensitive substring search in a line of known length (hl >= nl >= 1):
 * scan for the needle's first byte, then compare the rest. */
static BOOL contains(const char *hay, int hl, const char *needle, int nl)
{
    const char *p = hay, *last = hay + hl - nl;
    char f = needle[0];

    for (;;) {
        while (p <= last && *p != f) p++;
        if (p > last) return FALSE;
        if (nl == 1 || memcmp(p + 1, needle + 1, nl - 1) == 0) return TRUE;
        p++;
    }
}

/* case-insensitive variant (A-Z folding only, as to_lower); the needle is
 * already lower-case. The first-byte scan tests both cases of that byte, so
 * only candidate positions pay for folding the rest. */
static BOOL contains_ci(const char *hay, int hl, const char *needle, int nl)
{
    const char *p = hay, *last = hay + hl - nl;
    char f = needle[0], fu = f;
    int k;

    if ((unsigned char)(f - 'a') < 26) fu = (char)(f - 32);
    for (;;) {
        while (p <= last && *p != f && *p != fu) p++;
        if (p > last) return FALSE;
        for (k = 1; k < nl; k++)
            if (to_lower(p[k]) != needle[k]) break;
        if (k == nl) return TRUE;
        p++;
    }
}

/* ------------------------------------------------------------------ */
/* matcher                                                            */
/* ------------------------------------------------------------------ */

BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern, BOOL caseSensitive)
{
    char wrapped[260];
    LONG r;
    int  pl = (int)strlen((const char *)pattern);

    m->caseSensitive = caseSensitive;

    /* does the raw pattern contain AmigaDOS wildcard tokens? Probe with a
     * parse of the pattern as-is; r==1 means wildcards are present. */
    {
        UBYTE probe[600];
        LONG p = caseSensitive
               ? ParsePattern   ((CONST_STRPTR)pattern, (STRPTR)probe, (LONG)sizeof probe)
               : ParsePatternNoCase((CONST_STRPTR)pattern, (STRPTR)probe, (LONG)sizeof probe);
        if (p < 0) return FALSE;              /* malformed / too long          */
        m->useWild = (p == 1);
    }

    if (m->useWild) {
        /* wrap as "#?<pat>#?" so a whole-line MatchPattern behaves as a
         * "contains" test (grep semantics: match anywhere on the line). */
        if (pl + 4 >= (int)sizeof wrapped) return FALSE;
        wrapped[0] = '#'; wrapped[1] = '?';
        memcpy(wrapped + 2, (const char *)pattern, pl);
        wrapped[pl + 2] = '#'; wrapped[pl + 3] = '?';
        wrapped[pl + 4] = '\0';
        r = caseSensitive
          ? ParsePattern   ((CONST_STRPTR)wrapped, (STRPTR)m->parsed, (LONG)sizeof m->parsed)
          : ParsePatternNoCase((CONST_STRPTR)wrapped, (STRPTR)m->parsed, (LONG)sizeof m->parsed);
        if (r < 0) return FALSE;
        m->needle[0] = '\0';
        m->needleLen = 0;
    } else {
        if (pl >= (int)sizeof m->needle) return FALSE;
        if (caseSensitive) strcpy(m->needle, (const char *)pattern);
        else               str_lower(m->needle, pattern, (int)sizeof m->needle);
        m->needleLen = (int)strlen(m->needle);
    }
    return TRUE;
}

/* line: len bytes, NUL-terminated, no embedded NULs */
static BOOL lineMatches(struct Matcher *m, CONST_STRPTR line, int len)
{
    if (m->useWild) {
        return (m->caseSensitive
                ? MatchPattern      ((STRPTR)m->parsed, (STRPTR)line)
                : MatchPatternNoCase((STRPTR)m->parsed, (STRPTR)line)) ? TRUE : FALSE;
    }
    if (m->needleLen == 0) return TRUE;       /* empty needle matches all      */
    if (len < m->needleLen) return FALSE;
    if (m->caseSensitive)
        return contains((const char *)line, len, m->needle, m->needleLen);
    return contains_ci((const char *)line, len, m->needle, m->needleLen);
}

/* ------------------------------------------------------------------ */
/* filename filter                                                    */
/* ------------------------------------------------------------------ */

BOOL nameFilterInit(struct NameFilter *nf, CONST_STRPTR pat)
{
    UBYTE probe[600];
    char  wrapped[260];
    LONG  r;
    int   pl;

    if (!pat || pat[0] == '\0') { nf->active = FALSE; return TRUE; }
    pl = (int)strlen((const char *)pat);

    /* Probe: does the filter contain AmigaDOS wildcards? r==1 -> yes. */
    r = ParsePatternNoCase((CONST_STRPTR)pat, (STRPTR)probe, (LONG)sizeof probe);
    if (r < 0) { nf->active = FALSE; return FALSE; }

    if (r == 1) {
        /* wildcards present: match the whole name against the pattern as-is.
         * Re-parse directly into nf->parsed: copying a truncated token stream
         * (probe is bigger than nf->parsed) would hand MatchPatternNoCase an
         * unterminated buffer -> undefined behaviour. This way an over-long
         * pattern fails loudly instead. */
        r = ParsePatternNoCase((CONST_STRPTR)pat, (STRPTR)nf->parsed, (LONG)sizeof nf->parsed);
        if (r < 0) { nf->active = FALSE; return FALSE; }
    } else {
        /* plain text: wrap as #?<pat>#? so it matches any name *containing* it
         * (mirrors the Find field's substring behaviour). */
        if (pl + 4 >= (int)sizeof wrapped) { nf->active = FALSE; return FALSE; }
        wrapped[0] = '#'; wrapped[1] = '?';
        memcpy(wrapped + 2, (const char *)pat, pl);
        wrapped[pl + 2] = '#'; wrapped[pl + 3] = '?';
        wrapped[pl + 4] = '\0';
        r = ParsePatternNoCase((CONST_STRPTR)wrapped, (STRPTR)nf->parsed, (LONG)sizeof nf->parsed);
        if (r < 0) { nf->active = FALSE; return FALSE; }
    }
    nf->active = TRUE;
    return TRUE;
}

BOOL nameFilterMatch(struct NameFilter *nf, CONST_STRPTR name)
{
    if (!nf->active) return TRUE;
    return MatchPatternNoCase((STRPTR)nf->parsed, (STRPTR)name) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* one file                                                           */
/* ------------------------------------------------------------------ */

struct Ctx {
    struct Matcher    *m;
    struct NameFilter *nf;
    MatchFunc          found;
    APTR               foundUser;
    PollFunc           poll;
    APTR               pollUser;
    struct GrepStats  *st;
    char               path[520];           /* current path, grown/shrunk     */
    char               buf[8192];            /* file read buffer               */
    char               line[GREP_MAXLINE];   /* line accumulator               */
    BOOL               stop;
    ULONG              pollCounter;
};

/* Search one file: 'name' relative to directory lock 'dir' (c->path holds
 * its full name for reporting), or c->path itself when dir is 0 (the search
 * root is a plain file). Counts/reports via c->st. */
static void grepOneFile(struct Ctx *c, BPTR dir, CONST_STRPTR name)
{
    BPTR fh;
    LONG n;
    char *lp   = c->line;               /* end of the line gathered so far */
    char *lmax = c->line + GREP_MAXLINE - 1;
    ULONG lineNo = 1;
    BOOL fileMatched = FALSE;
    BOOL sniffed = FALSE;        /* have we classified the file yet?            */

    if (dir) {                   /* relative open: no path walk from the root   */
        BPTR old = CurrentDir(dir);
        fh = Open((STRPTR)name, MODE_OLDFILE);
        CurrentDir(old);
    } else {
        fh = Open((STRPTR)c->path, MODE_OLDFILE);
    }
    if (!fh) return;

    while (!c->stop && (n = Read(fh, c->buf, (LONG)sizeof c->buf)) > 0) {
        const char *p, *end;

        /* M4: decide text-vs-binary up front, from the FIRST block only (grep's
         * first-buffer heuristic). If that block holds a NUL the file is binary,
         * so skip it whole - before any match is reported - and the stats stay
         * consistent (no "N line(s) in 0 file(s)"). A NUL in a *later* block is
         * dropped as noise below; the file has already been judged text. */
        if (!sniffed) {
            LONG k;
            for (k = 0; k < n; k++) {
                if (c->buf[k] == '\0') {         /* binary: skip the whole file */
                    Close(fh);
                    c->st->binarySkipped++;
                    return;
                }
            }
            c->st->filesScanned++;
            sniffed = TRUE;
        }

        for (p = c->buf, end = c->buf + n; p < end; p++) {
            char ch = *p;

            /* fast path: every byte above CR is plain line text */
            if ((unsigned char)ch > '\r') {
                /* keep scanning past GREP_MAXLINE, silently dropping the excess */
                if (lp < lmax) *lp++ = ch;
                continue;
            }

            if (ch == '\0') continue;            /* stray NUL in a text file     */

            if (ch == '\n') {
                int linePos = (int)(lp - c->line);
                *lp = '\0';
                if (lineMatches(c->m, (CONST_STRPTR)c->line, linePos)) {
                    c->st->linesMatched++;
                    fileMatched = TRUE;
                    if (c->st->namesOnly) {
                        if (c->found && !c->found(c->path, 0, (CONST_STRPTR)"", c->foundUser))
                            c->stop = TRUE;
                        goto done;               /* one hit is enough           */
                    }
                    if (c->found && !c->found(c->path, lineNo, (CONST_STRPTR)c->line,
                                              c->foundUser))
                        c->stop = TRUE;
                }
                lineNo++; lp = c->line;
                if (c->stop) break;

                if (c->poll && ((++c->pollCounter & 63) == 0)) {
                    if (c->poll(c->pollUser)) { c->stop = TRUE; break; }
                }
            } else if (ch != '\r') {             /* TAB and other low controls   */
                if (lp < lmax) *lp++ = ch;
            }
        }

        /* M3: poll once per block too. A file made of very long lines (or no
         * newline at all) would otherwise never reach the per-newline poll, so
         * Stop/Ctrl-C was ignored and the GUI froze for the whole file. */
        if (!c->stop && c->poll && c->poll(c->pollUser)) c->stop = TRUE;
    }

    /* trailing line with no newline */
    if (!c->stop && sniffed && lp > c->line) {
        *lp = '\0';
        if (lineMatches(c->m, (CONST_STRPTR)c->line, (int)(lp - c->line))) {
            c->st->linesMatched++;
            fileMatched = TRUE;
            if (c->found) {
                if (c->st->namesOnly) c->found(c->path, 0, (CONST_STRPTR)"", c->foundUser);
                else                  c->found(c->path, lineNo, (CONST_STRPTR)c->line,
                                                c->foundUser);
            }
        }
    }

    /* an empty file (or one whose first Read failed) opened fine but was never
     * sniffed; count it as a scanned text file with no matches, as before. */
    if (!sniffed) c->st->filesScanned++;

done:
    Close(fh);
    if (fileMatched) c->st->filesMatched++;
}

/* ------------------------------------------------------------------ */
/* recursive walk                                                     */
/* ------------------------------------------------------------------ */

/* ExAll buffer per directory level (long-aligned via AllocVec). 4 KB holds
 * ~150 ED_TYPE entries, so a typical drawer is read in one handler call. */
#define EXALL_BUFSIZE 4096

static void walkDir(struct Ctx *c, BPTR dir);

/* A soft link: resolve it and grep it if it points at a file. Links to
 * directories are never followed - one pointing at an ancestor would loop
 * (bounded only by the path cap) and one pointing outside the root would
 * re-scan unrelated trees with duplicate hits. */
static void softLink(struct Ctx *c, BPTR dir, CONST_STRPTR name)
{
    struct FileInfoBlock *fib;
    BPTR old, lk;

    old = CurrentDir(dir);
    lk  = Lock((STRPTR)name, ACCESS_READ);
    CurrentDir(old);
    if (!lk) return;                     /* dangling link                */

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        /* filter on the target's name, as the old full-path Lock did */
        if (Examine(lk, fib) && fib->fib_DirEntryType < 0 &&
            nameFilterMatch(c->nf, fib->fib_FileName))
            grepOneFile(c, dir, name);
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lk);
}

/* One directory entry, named relative to its parent lock 'dir'. c->path is
 * only built for reporting; the filesystem is always addressed relative to
 * 'dir', so DOS never re-walks the full path from the root. */
static void handleEntry(struct Ctx *c, BPTR dir, CONST_STRPTR name, LONG type)
{
    LONG plen = (LONG)strlen(c->path);

    if (!AddPart((STRPTR)c->path, (STRPTR)name, (ULONG)sizeof c->path)) {
        c->path[plen] = '\0';            /* AddPart may have mutated the
                                          * buffer before failing; restore
                                          * or the rest of this dir scans
                                          * against a corrupted base path */
        return;                          /* full path would overflow    */
    }

    if (type == ST_SOFTLINK) {
        softLink(c, dir, name);
    } else if (type == ST_LINKDIR) {
        /* hard link to a directory: not followed (see softLink) */
    } else if (type > 0) {
        BPTR old, child;
        c->st->dirsScanned++;
        old   = CurrentDir(dir);
        child = Lock((STRPTR)name, ACCESS_READ);
        CurrentDir(old);
        if (child) {
            walkDir(c, child);
            UnLock(child);
        }
    } else {
        if (nameFilterMatch(c->nf, name))
            grepOneFile(c, dir, name);
    }

    c->path[plen] = '\0';                /* restore parent path         */
}

/* per-entry poll, same cadence as before (every 64 entries) */
static BOOL pollStop(struct Ctx *c)
{
    if (c->poll && ((++c->pollCounter & 63) == 0)) {
        if (c->poll(c->pollUser)) c->stop = TRUE;
    }
    return c->stop;
}

/* Fallback enumeration with Examine/ExNext, for handlers that don't know
 * ACTION_EXAMINE_ALL (and for when the ExAll buffers can't be allocated). */
static void walkDirExNext(struct Ctx *c, BPTR dir)
{
    struct FileInfoBlock *fib =
        (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;

    if (Examine(dir, fib)) {
        while (!c->stop && ExNext(dir, fib)) {
            if (pollStop(c)) break;
            handleEntry(c, dir, (CONST_STRPTR)fib->fib_FileName,
                        fib->fib_DirEntryType);
        }
    }
    FreeDosObject(DOS_FIB, fib);
}

/* Enumerate a directory with ExAll (one handler call per batch of entries
 * instead of one ExNext per entry), in the handler's own ExNext order. */
static void walkDir(struct Ctx *c, BPTR dir)
{
    struct ExAllControl *eac;
    struct ExAllData    *buf, *ead;
    BOOL more, first = TRUE, fallback = FALSE;
    LONG err;

    if (c->stop) return;

    eac = (struct ExAllControl *)AllocDosObject(DOS_EXALLCONTROL, NULL);
    buf = (struct ExAllData *)AllocVec(EXALL_BUFSIZE, MEMF_ANY);
    if (!eac || !buf) {
        if (buf) FreeVec(buf);
        if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);
        walkDirExNext(c, dir);
        return;
    }
    eac->eac_LastKey = 0;

    do {
        SetIoErr(0);
        more = ExAll(dir, buf, EXALL_BUFSIZE, ED_TYPE, eac);
        err  = more ? 0 : IoErr();
        if (!more && err != ERROR_NO_MORE_ENTRIES) {
            /* Failed before returning anything: ERROR_ACTION_NOT_KNOWN from
             * an old/odd handler (some answer with other codes), so redo the
             * directory the classic way. A later failure just ends it. */
            if (first && eac->eac_Entries == 0) fallback = TRUE;
            break;
        }
        first = FALSE;
        if (eac->eac_Entries == 0) continue;

        for (ead = buf; ead; ead = ead->ed_Next) {
            if (pollStop(c)) break;
            handleEntry(c, dir, (CONST_STRPTR)ead->ed_Name, ead->ed_Type);
            if (c->stop) break;
        }
    } while (more && !c->stop);

    /* stopped with the scan still open: tell the handler to drop its state
     * (ExAllEnd is V39+; on V37 drain the remaining batches instead) */
    if (more) {
        if (DOSBase->dl_lib.lib_Version >= 39)
            ExAllEnd(dir, buf, EXALL_BUFSIZE, ED_TYPE, eac);
        else
            while (ExAll(dir, buf, EXALL_BUFSIZE, ED_TYPE, eac)) ;
    }

    FreeVec(buf);
    FreeDosObject(DOS_EXALLCONTROL, eac);

    if (fallback) walkDirExNext(c, dir);
}

static void recurse(struct Ctx *c, BPTR lock)
{
    struct FileInfoBlock *fib;

    if (c->stop) return;

    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;

    if (Examine(lock, fib)) {
        if (fib->fib_DirEntryType > 0) {
            walkDir(c, lock);            /* directory -> enumerate children */
        } else {
            /* the root itself is a plain file */
            if (nameFilterMatch(c->nf, fib->fib_FileName))
                grepOneFile(c, 0, NULL);
        }
    }

    FreeDosObject(DOS_FIB, fib);
}

LONG grepTree(CONST_STRPTR rootPath, struct Matcher *m, struct NameFilter *nf,
              MatchFunc found, APTR foundUser,
              PollFunc  poll,  APTR pollUser,
              struct GrepStats *stats)
{
    struct Ctx *c;
    BPTR lock;

    /* Ctx is large (read buffer + path + line); keep it off the stack so deep
     * recursion stays cheap. */
    c = (struct Ctx *)AllocMem(sizeof(struct Ctx), MEMF_CLEAR);
    if (!c) return ERROR_NO_FREE_STORE;

    c->m = m;          c->nf = nf;
    c->found = found;  c->foundUser = foundUser;
    c->poll  = poll;   c->pollUser  = pollUser;
    c->st = stats;

    stats->dirsScanned = stats->filesScanned = stats->filesMatched = 0;
    stats->linesMatched = stats->binarySkipped = 0;
    stats->aborted = FALSE;

    strncpy(c->path, (const char *)rootPath, sizeof c->path - 1);
    c->path[sizeof c->path - 1] = '\0';

    lock = Lock((STRPTR)rootPath, ACCESS_READ);
    if (!lock) {
        LONG err = IoErr();
        FreeMem(c, sizeof(struct Ctx));
        return err;
    }

    recurse(c, lock);
    UnLock(lock);

    stats->aborted = c->stop;
    FreeMem(c, sizeof(struct Ctx));
    return 0;
}
