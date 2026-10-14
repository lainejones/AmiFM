/*
 * AmiFind - shared file search engine (see finder.h)
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <string.h>

#include "finder.h"

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

static BOOL substr_ci(CONST_STRPTR haystackRaw, CONST_STRPTR needleLower)
{
    char hay[140];
    int nl, hl, i;

    str_lower(hay, haystackRaw, (int)sizeof hay);
    nl = (int)strlen(needleLower);
    if (nl == 0) return TRUE;               /* empty pattern matches all  */
    hl = (int)strlen(hay);
    for (i = 0; i + nl <= hl; i++) {
        if (strncmp(hay + i, needleLower, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* matcher                                                            */
/* ------------------------------------------------------------------ */

BOOL matcherInit(struct Matcher *m, CONST_STRPTR pattern)
{
    LONG r = ParsePatternNoCase((CONST_STRPTR)pattern,
                                (STRPTR)m->parsed, (LONG)sizeof m->parsed);
    if (r < 0) return FALSE;                /* pattern too long / bad     */
    m->useWild = (r == 1);                  /* 1 -> wildcards present     */
    str_lower(m->lower, pattern, (int)sizeof m->lower);
    return TRUE;
}

BOOL matcherMatch(struct Matcher *m, CONST_STRPTR name)
{
    if (m->useWild)
        return MatchPatternNoCase((STRPTR)m->parsed, (STRPTR)name) ? TRUE : FALSE;
    return substr_ci(name, (CONST_STRPTR)m->lower);
}

/* ------------------------------------------------------------------ */
/* recursive walk                                                     */
/* ------------------------------------------------------------------ */

struct Ctx {
    struct Matcher    *m;
    FoundFunc          found;
    APTR               foundUser;
    PollFunc           poll;
    APTR               pollUser;
    struct SearchStats *st;
    char               path[520];           /* current path, grown/shrunk */
    BOOL               stop;
    ULONG              pollCounter;
};

/* ExAll buffer per directory level (long-aligned via AllocVec). 4 KB holds
 * ~100 ED_SIZE entries, so a typical drawer is read in one handler call. */
#define EXALL_BUFSIZE 4096

static void walkDir(struct Ctx *c, BPTR dir);

/* One directory entry, named relative to its parent lock 'dir'.
 * c->path is only built for reporting; the filesystem is always addressed
 * relative to 'dir', so DOS never re-walks the full path from the root. */
static void handleEntry(struct Ctx *c, BPTR dir, CONST_STRPTR name,
                        LONG type, LONG size)
{
    LONG plen = (LONG)strlen(c->path);

    if (!AddPart((STRPTR)c->path, (STRPTR)name, (ULONG)sizeof c->path)) {
        c->path[plen] = '\0';            /* AddPart may have mutated the
                                          * buffer before failing; restore
                                          * or the rest of this dir scans
                                          * against a corrupted base path */
        return;                          /* full path would overflow   */
    }

    if (type > 0) {
        c->st->dirsScanned++;
        if (matcherMatch(c->m, name)) {
            c->st->matches++;
            if (!c->found(c->path, TRUE, size, c->foundUser))
                c->stop = TRUE;
        }
        /* Report links but never recurse into them: a soft link or
         * link-dir can point outside the root (re-scanning a whole
         * volume, duplicate hits) or back at an ancestor (loops
         * until AddPart caps the path). Only descend real dirs. */
        if (!c->stop && type != ST_SOFTLINK && type != ST_LINKDIR) {
            BPTR old   = CurrentDir(dir);
            BPTR child = Lock((STRPTR)name, ACCESS_READ);
            CurrentDir(old);
            if (child) {
                walkDir(c, child);
                UnLock(child);
            }
        }
    } else {
        c->st->filesScanned++;
        if (matcherMatch(c->m, name)) {
            c->st->matches++;
            if (!c->found(c->path, FALSE, size, c->foundUser))
                c->stop = TRUE;
        }
    }

    c->path[plen] = '\0';                /* restore parent path        */
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
    /* A FileInfoBlock must be long-word aligned; AllocDosObject guarantees
     * that and keeps it off our (limited) stack on every recursion level. */
    struct FileInfoBlock *fib =
        (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) return;

    if (Examine(dir, fib)) {
        while (!c->stop && ExNext(dir, fib)) {
            if (pollStop(c)) break;
            handleEntry(c, dir, (CONST_STRPTR)fib->fib_FileName,
                        fib->fib_DirEntryType, fib->fib_Size);
        }
    }
    FreeDosObject(DOS_FIB, fib);
}

/* Enumerate a directory with ExAll (one handler call per ~100 entries
 * instead of one ExNext per entry). Entry order is the handler's own, the
 * same order ExNext returns. */
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
        more = ExAll(dir, buf, EXALL_BUFSIZE, ED_SIZE, eac);
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
            handleEntry(c, dir, (CONST_STRPTR)ead->ed_Name,
                        ead->ed_Type, (LONG)ead->ed_Size);
            if (c->stop) break;
        }
    } while (more && !c->stop);

    /* stopped with the scan still open: tell the handler to drop its state
     * (ExAllEnd is V39+; on V37 drain the remaining batches instead) */
    if (more) {
        if (DOSBase->dl_lib.lib_Version >= 39)
            ExAllEnd(dir, buf, EXALL_BUFSIZE, ED_SIZE, eac);
        else
            while (ExAll(dir, buf, EXALL_BUFSIZE, ED_SIZE, eac)) ;
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
            c->st->filesScanned++;
            if (matcherMatch(c->m, fib->fib_FileName)) {
                c->st->matches++;
                if (!c->found(c->path, FALSE, fib->fib_Size, c->foundUser))
                    c->stop = TRUE;
            }
        }
    }

    FreeDosObject(DOS_FIB, fib);
}

LONG searchTree(CONST_STRPTR rootPath, struct Matcher *m,
                FoundFunc found, APTR foundUser,
                PollFunc  poll,  APTR pollUser,
                struct SearchStats *stats)
{
    struct Ctx c;
    BPTR lock;

    memset(&c, 0, sizeof c);
    c.m = m;
    c.found = found;   c.foundUser = foundUser;
    c.poll  = poll;    c.pollUser  = pollUser;
    c.st = stats;

    stats->matches = stats->dirsScanned = stats->filesScanned = 0;
    stats->aborted = FALSE;

    strncpy(c.path, (const char *)rootPath, sizeof c.path - 1);
    c.path[sizeof c.path - 1] = '\0';

    lock = Lock((STRPTR)rootPath, ACCESS_READ);
    if (!lock) {
        LONG e = IoErr();            /* never return 0 (= success) on a failed
                                      * Lock - a handler that forgets to set
                                      * IoErr would otherwise look like 0 hits */
        return e ? e : ERROR_OBJECT_NOT_FOUND;
    }

    recurse(&c, lock);
    UnLock(lock);

    stats->aborted = c.stop;
    return 0;
}
