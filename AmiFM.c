/*
 * AmiFM - dual-pane file manager for AmigaOS 3.x  [custom-lister build]
 *
 * Custom-drawn listers: rows are rendered by hand, so columns are pinned to
 * exact pixels (Name | Size | Date) with headers, right-aligned size, and no
 * clipping. Custom scrollbars per pane handle long lists. Left-click selects;
 * left double-click does the type-aware action (enter drawer / extract archive
 * / view file). The RIGHT button opens the menu strip (incl. Iconify).
 *
 * Build:  `make` (AmiFM.c + amigui.c + engine/, -Os -noixemul -Wall -s)
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxmacros.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/datetime.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>   /* varargs for our tiny fmt() - NOT stdio */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/wb.h>
#include <proto/icon.h>

#include "finder.h"     /* AmiFind's search engine (engine/) - Find */
#include "grepeng.h"    /* AmiGrep's, renamed (engine/) - Grep */
#include "amigui.h"     /* shared About box, settings page frame, ENV:/ENVARC: writing */

#define AMIFM_VERSION "1.2.2"
/* what About says about the licence (the LICENSE file: GPL-3.0) */
#define AMIFM_LICENCE "Licence: GPL-3.0"

/* AmigaDOS version cookie (the C: Version command / $VER reads this) */
static const char verstag[] __attribute__((used)) =
    "$VER: AmiFM " AMIFM_VERSION " (30.10.2026)";

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *WorkbenchBase = NULL;
struct Library       *IconBase      = NULL;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct Screen   *scr;
unsigned long __stack = 20000;         /* ensure a roomy stack (startup honours this) */

static struct Window   *win;
static struct DrawInfo *dri;
static struct TextFont *font;
static APTR             vi;

static WORD g_rowH, g_base, g_cw;     /* row height, baseline, char width */
static UWORD pTEXT, pBG, pFILL, pFTXT, pSHADOW, pSHINE;
static int  g_active = 0;             /* focused pane: source for ops, other = dest */
static const char *toolLabels[] = {
    "View",    "Edit",    "Copy", "Move", "Rename", "Delete", "MakeDir",   /* row 0 */
    "AddIcon", "Extract", "Pack", "Find", "Grep", "Swap", "All", "None"    /* row 1 */
};
#define NBTN 15
static const UBYTE toolRow[NBTN] = { 0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1 };
static WORD g_btnX[NBTN], g_btnYr[NBTN], g_btnWr[NBTN], g_btnH, g_btnTop;   /* custom buttons, stacked rows */

static struct Menu *g_menu = NULL;     /* right-button menu strip */

/* fallback AppIcon image (used only if PROGDIR:AmiFM.info can't be loaded) */
static UWORD g_appIconBits[16] = {
    0xFFFF, 0x8001, 0xFFFF, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001,
    0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0xFFFF
};
static struct Image g_appIconImg = { 0, 0, 16, 16, 1, g_appIconBits, 0x1, 0x0, NULL };
static struct DiskObject g_appIconDO;

/* fallback drawer icon (Add Icon, used only if GetDefDiskObject(WBDRAWER) is NULL
 * on a minimal system) - a simple folder outline */
static UWORD g_drawerBits[12] = {
    0x0000, 0x3C00, 0x4200, 0x7FFE, 0x4002, 0x4002,
    0x4002, 0x4002, 0x4002, 0x4002, 0x7FFE, 0x0000
};
static struct Image g_drawerImg = { 0, 0, 16, 12, 1, g_drawerBits, 0x1, 0x0, NULL };
static struct DiskObject g_drawerDO;
static struct DrawerData g_drawerDD;

struct Entry { char name[108]; char sizestr[16]; char datestr[16]; LONG size; LONG dkey; BOOL isdir; BOOL tagged; };

struct Pane {
    char  path[256];
    struct Entry *entries;
    int   count, top, sel;
    WORD  rx, ry, rw, rh;                 /* list draw region */
    WORD  py;                             /* path-bar row y */
    WORD  hy;                             /* header row y */
    WORD  colName, colSizeBeg, colSizeEnd;
    int   nameChars, visRows;
    WORD  sbx, sby, sbw, sbh;             /* custom scrollbar track (right of list) */
    ULONG lastsec, lastmic; int lastrow;
    int   sortBy, sortRev;                /* 0=name 1=size 2=date; rev=descending */
};
static struct Pane panes[2];

static int g_sortBy, g_sortRev;

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Minimal bounded formatter (replaces snprintf to avoid pulling in the whole
 * stdio/dtoa machinery). Handles %s, %d, %ld, %% only. Always NUL-terminates. */
static void fmt(char *out, int outsz, const char *f, ...)
{
    va_list ap;
    char *o = out, *end = out + (outsz > 0 ? outsz - 1 : 0);
    va_start(ap, f);
    while (*f && o < end) {
        if (*f != '%') { *o++ = *f++; continue; }
        f++;
        if (*f == 'l') f++;                        /* int and long are both 32-bit here */
        if (*f == 'd') {
            long v = va_arg(ap, long); unsigned long u; char tmp[12]; int ti = 0;
            if (v < 0) { if (o < end) *o++ = '-'; u = (unsigned long)(-v); } else u = (unsigned long)v;
            do { tmp[ti++] = (char)('0' + (u % 10)); u /= 10; } while (u && ti < 11);
            while (ti && o < end) *o++ = tmp[--ti];
            f++;
        } else if (*f == 's') {
            const char *s = va_arg(ap, const char *); if (!s) s = "";
            while (*s && o < end) *o++ = *s++;
            f++;
        } else if (*f == '%') { *o++ = '%'; f++; }
        else if (*f) { *o++ = *f++; }
    }
    *o = '\0';
    va_end(ap);
}

/* ---- settings ------------------------------------------------------------
 * What AmiFM can be set to, and where each value comes from, lowest first:
 *   1. the built-in defaults (setsDefault below);
 *   2. ENV:EDITOR - only for Edit, and only while the Edit setting is empty;
 *   3. the settings file AmiFM.prefs: ENV:AmiFM.prefs, else ENVARC:AmiFM.prefs
 *      (Settings > ... > Save writes both, Use writes ENV: only, so a Use
 *      lasts until the next reboot). It is plain text, one KEY=value a line:
 *      LEFT, RIGHT (the start drawers), VIEW, EDIT, EXTRACT, PACK (the programs),
 *      WINDOW=x,y,w,h (the main window's place and size, Settings > Snapshot
 *      window; none = a window that fits one 800-wide monitor).
 * AmiFM reads no tooltypes, so there are none for the file to override. */

#define PREFS_NAME "AmiFM.prefs"

struct Settings {
    char left[256], right[256];        /* the panes' start drawers */
    char view[120];                    /* View:    <view> "<file>" */
    char edit[120];                    /* Edit:    <edit> "<file>"; empty = ENV:EDITOR, else TextEdit */
    char extract[120];                 /* Extract: <extract> "<archive>" DEST "<drawer>" OVERWRITE */
    char pack[120];                    /* Pack:    <pack> a "<name>.lha" "<item>" */
    long winx, winy, winw, winh;       /* the main window, snapshot; winw 0 = none */
};
static struct Settings g_set;

static void setStr(char *dst, int size, const char *v)
{
    strncpy(dst, v, size - 1); dst[size - 1] = '\0';
}

static void setsDefault(struct Settings *s)
{
    memset(s, 0, sizeof *s);
    strcpy(s->left, "SYS:");
    strcpy(s->right, "RAM:");
    strcpy(s->view, "SYS:Utilities/MultiView");
    s->edit[0] = '\0';
    strcpy(s->extract, "xadUnFile");
    strcpy(s->pack, "lha");
}

static void setsLoad(struct Settings *s)
{
    char buf[1200], *line, *next;
    setsDefault(s);
    if (amg_env_read(PREFS_NAME, buf, sizeof buf) < 0) return;
    for (line = buf; line && *line; line = next) {
        char *v;
        if ((next = strchr(line, '\n'))) *next++ = '\0';
        if ((v = strchr(line, '\r'))) *v = '\0';
        if (!(v = strchr(line, '='))) continue;
        *v++ = '\0';
        if      (!strcmp(line, "LEFT"))    { if (*v) setStr(s->left, sizeof s->left, v); }
        else if (!strcmp(line, "RIGHT"))   { if (*v) setStr(s->right, sizeof s->right, v); }
        else if (!strcmp(line, "VIEW"))    { if (*v) setStr(s->view, sizeof s->view, v); }
        else if (!strcmp(line, "EDIT"))    setStr(s->edit, sizeof s->edit, v);   /* empty = ENV:EDITOR */
        else if (!strcmp(line, "EXTRACT")) { if (*v) setStr(s->extract, sizeof s->extract, v); }
        else if (!strcmp(line, "PACK"))    { if (*v) setStr(s->pack, sizeof s->pack, v); }
        else if (!strcmp(line, "WINDOW")) {
            long n[4]; int k;
            for (k = 0; k < 4 && *v; k++) {
                n[k] = strtol(v, &v, 10);
                if (*v == ',') v++;
            }
            if (k == 4 && n[2] > 0 && n[3] > 0) {
                s->winx = n[0]; s->winy = n[1]; s->winw = n[2]; s->winh = n[3];
            }
        }
    }
}

/* keep = Save (ENV: + ENVARC:), else Use (ENV: only) */
static BOOL setsWrite(const struct Settings *s, BOOL keep)
{
    char buf[1200], ln[300];
    buf[0] = '\0';
    fmt(ln, sizeof ln, "LEFT=%s\n", s->left);       strcat(buf, ln);
    fmt(ln, sizeof ln, "RIGHT=%s\n", s->right);     strcat(buf, ln);
    fmt(ln, sizeof ln, "VIEW=%s\n", s->view);       strcat(buf, ln);
    fmt(ln, sizeof ln, "EDIT=%s\n", s->edit);       strcat(buf, ln);
    fmt(ln, sizeof ln, "EXTRACT=%s\n", s->extract); strcat(buf, ln);
    fmt(ln, sizeof ln, "PACK=%s\n", s->pack);       strcat(buf, ln);
    if (s->winw > 0) {
        fmt(ln, sizeof ln, "WINDOW=%d,%d,%d,%d\n", s->winx, s->winy, s->winw, s->winh);
        strcat(buf, ln);
    }
    return amg_env_write(PREFS_NAME, buf, strlen(buf), keep) ? TRUE : FALSE;
}

static int cmpEntry(const void *pa, const void *pb)
{
    const struct Entry *a = pa, *b = pb;
    int r;
    /* nav entries pin to the top: "." first, then ".." (Unix ls -a order) */
    #define NAVRANK(e) ((e)->name[0]=='.' ? ((e)->name[1]=='\0' ? 0 : ((e)->name[1]=='.'&&(e)->name[2]=='\0') ? 1 : 2) : 2)
    {
        int ra = NAVRANK(a), rb = NAVRANK(b);
        if (ra != rb) return ra - rb;
        if (ra < 2) return 0;            /* both are nav entries */
    }
    #undef NAVRANK
    if (a->isdir != b->isdir) return a->isdir ? -1 : 1;   /* drawers always grouped first */
    if (g_sortBy == 1)      r = (a->size > b->size) ? 1 : (a->size < b->size) ? -1 : 0;
    else if (g_sortBy == 2) r = (a->dkey > b->dkey) ? 1 : (a->dkey < b->dkey) ? -1 : 0;
    else                    r = ci_cmp(a->name, b->name);
    if (r == 0) r = ci_cmp(a->name, b->name);
    return g_sortRev ? -r : r;
}

static void sortPane(struct Pane *p)
{
    if (p->count > 1) {
        g_sortBy = p->sortBy; g_sortRev = p->sortRev;
        qsort(p->entries, p->count, sizeof(struct Entry), cmpEntry);
    }
    p->sel = -1;
}

static void freeEntries(struct Pane *p)
{
    if (p->entries) { FreeVec(p->entries); p->entries = NULL; }
    p->count = 0;
}

/* make room for 'need' entries: grow by doubling (AllocVec + CopyMem + FreeVec).
 * New slots are zeroed (MEMF_CLEAR). FALSE = out of memory, array unchanged. */
static BOOL growEntries(struct Pane *p, int *cap, int need)
{
    struct Entry *ne; int nc = *cap ? *cap : 64;
    if (need <= *cap) return TRUE;
    while (nc < need) nc *= 2;
    ne = AllocVec(sizeof(struct Entry) * nc, MEMF_CLEAR);
    if (!ne) return FALSE;
    if (p->entries) {
        CopyMem(p->entries, ne, sizeof(struct Entry) * p->count);
        FreeVec(p->entries);
    }
    p->entries = ne; *cap = nc;
    return TRUE;
}

/* append one row (type > 0 = drawer, ds NULL = no date); FALSE = out of memory */
static BOOL addEntry(struct Pane *p, int *cap, const char *name, LONG type, LONG size,
                     const struct DateStamp *ds)
{
    struct Entry *e;
    if (!growEntries(p, cap, p->count + 1)) return FALSE;
    e = &p->entries[p->count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->isdir = (type > 0);
    e->size  = size;
    if (e->isdir) strcpy(e->sizestr, "<DIR>");
    else fmt(e->sizestr, sizeof(e->sizestr), "%ld", (long)e->size);
    if (ds) {
        struct DateTime dt; char db[20]; db[0] = '\0';
        dt.dat_Stamp = *ds; dt.dat_Format = FORMAT_DOS; dt.dat_Flags = 0;
        dt.dat_StrDay = NULL; dt.dat_StrDate = (STRPTR)db; dt.dat_StrTime = NULL;
        DateToStr(&dt);
        strncpy(e->datestr, db, sizeof(e->datestr) - 1);
        e->dkey = ds->ds_Days * 1440L + ds->ds_Minute;
    }
    return TRUE;
}

#define EXALL_BUFSZ 4096

/* stop an ExAll scan early: ExAllEnd (dos V39+), else drain the remaining calls */
static void exAllStop(BPTR lock, struct ExAllData *buf, struct ExAllControl *eac)
{
    if (((struct Library *)DOSBase)->lib_Version >= 39)
        ExAllEnd(lock, buf, EXALL_BUFSZ, ED_DATE, eac);
    else
        while (ExAll(lock, buf, EXALL_BUFSZ, ED_DATE, eac)) ;
}

/* Read a drawer in ONE pass: ExAll (name/type/size/date) into a growable array,
 * falling back to Examine/ExNext if the handler doesn't know ExAll. */
static void scanPane(struct Pane *p)
{
    BPTR lock; int cap = 0; BOOL useExNext = TRUE, oom = FALSE;
    struct Process *pr = (struct Process *)FindTask(NULL);
    APTR oldwp = pr->pr_WindowPtr;
    pr->pr_WindowPtr = (APTR)-1L;          /* suppress "insert volume" requesters (e.g. empty DF0:) */
    freeEntries(p); p->top = 0; p->sel = -1;

    lock = Lock((STRPTR)p->path, ACCESS_READ);
    if (!lock) { pr->pr_WindowPtr = oldwp; return; }
    {
        struct ExAllControl *eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
        struct ExAllData *buf = AllocVec(EXALL_BUFSZ, MEMF_ANY);
        if (eac && buf) {
            BOOL more;
            useExNext = FALSE;
            eac->eac_LastKey = 0; eac->eac_MatchString = NULL; eac->eac_MatchFunc = NULL;
            do {
                struct ExAllData *ed;
                more = ExAll(lock, buf, EXALL_BUFSZ, ED_DATE, eac);
                if (!more && IoErr() == ERROR_ACTION_NOT_KNOWN && p->count == 0) {
                    useExNext = TRUE; break;          /* handler has no ExAll: use ExNext */
                }
                for (ed = (eac->eac_Entries > 0) ? buf : NULL; ed; ed = ed->ed_Next) {
                    struct DateStamp ds;
                    ds.ds_Days = ed->ed_Days; ds.ds_Minute = ed->ed_Mins; ds.ds_Tick = ed->ed_Ticks;
                    if (!addEntry(p, &cap, (char *)ed->ed_Name, ed->ed_Type, (LONG)ed->ed_Size, &ds))
                        { oom = TRUE; break; }
                }
                if (oom && more) { exAllStop(lock, buf, eac); more = FALSE; }
            } while (more);
        }
        if (buf) FreeVec(buf);
        if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);
    }
    if (useExNext && !oom) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib && Examine(lock, fib))
            while (ExNext(lock, fib))
                if (!addEntry(p, &cap, (char *)fib->fib_FileName, fib->fib_DirEntryType,
                              fib->fib_Size, &fib->fib_Date)) break;
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    pr->pr_WindowPtr = oldwp;
    /* the "." (reload) and ".." (parent) nav entries, Unix-style (sort pins them on top) */
    addEntry(p, &cap, ".", 1, 0, NULL);
    addEntry(p, &cap, "..", 1, 0, NULL);
    sortPane(p);
}

/* raised (or recessed) bevel box, button/knob look */
static void bevelBox(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h, BOOL raised)
{
    SetAPen(rp, pBG); RectFill(rp, x, y, x + w - 1, y + h - 1);
    SetAPen(rp, raised ? pSHINE : pSHADOW);
    Move(rp, x, y + h - 1); Draw(rp, x, y); Draw(rp, x + w - 1, y);
    SetAPen(rp, raised ? pSHADOW : pSHINE);
    Draw(rp, x + w - 1, y + h - 1); Draw(rp, x, y + h - 1);
}

/* custom scrollbar: up/down arrow buttons + proportional knob (any window/rastport) */
static void drawScrollbarAt(struct RastPort *rp, WORD sbx, WORD sby, WORD sbw, WORD sbh,
                            int count, int visRows, int top)
{
    WORD ah = sbw;                                     /* square arrow buttons */
    WORD trackTop = sby + ah, trackH = sbh - 2 * ah;
    int  maxtop = count - visRows; if (maxtop < 0) maxtop = 0;
    WORD knobY, knobH, ty = (ah - g_rowH) / 2 + g_base;
    if (trackH < 4) return;
    SetAPen(rp, pSHADOW);                              /* recessed track well */
    RectFill(rp, sbx, trackTop, sbx + sbw - 1, trackTop + trackH - 1);
    if (count <= visRows || maxtop == 0) { knobH = trackH; knobY = trackTop; }
    else {
        knobH = (WORD)((long)trackH * visRows / count); if (knobH < 8) knobH = 8;
        knobY = trackTop + (WORD)((long)(trackH - knobH) * top / maxtop);
    }
    bevelBox(rp, sbx, knobY, sbw, knobH, TRUE);        /* knob */
    bevelBox(rp, sbx, sby, sbw, ah, TRUE);             /* up arrow */
    bevelBox(rp, sbx, sby + sbh - ah, sbw, ah, TRUE);  /* down arrow */
    SetAPen(rp, pTEXT); SetDrMd(rp, JAM1);
    Move(rp, sbx + (sbw - g_cw) / 2, sby + ty);              Text(rp, (STRPTR)"^", 1);
    Move(rp, sbx + (sbw - g_cw) / 2, sby + sbh - ah + ty);   Text(rp, (STRPTR)"v", 1);
}

static void drawScrollbar(struct Pane *p)
{
    drawScrollbarAt(win->RPort, p->sbx, p->sby, p->sbw, p->sbh, p->count, p->visRows, p->top);
}

/* small raised glyph button that fills the empty square above the scrollbar:
 * kind 0 = Parent (up arrow), kind 1 = Reload (clockwise circular arrow) */
static void drawPaneGadget(WORD x, WORD y, WORD w, WORD h, int kind)
{
    struct RastPort *rp = win->RPort;
    WORD cx = x + w / 2;
    bevelBox(rp, x, y, w, h, TRUE);
    SetAPen(rp, pTEXT); SetDrMd(rp, JAM1);
    if (kind == 0) {                          /* Parent: up arrow */
        Move(rp, cx, y + h - 2); Draw(rp, cx, y + 2);
        Move(rp, cx - 2, y + 4); Draw(rp, cx, y + 2); Draw(rp, cx + 2, y + 4);
    } else {                                  /* Reload: two opposing arrows (refresh/cycle) */
        Move(rp, cx - 3, y + 2); Draw(rp, cx + 2, y + 2);          /* top shaft, points right */
        Move(rp, cx + 1, y + 1); Draw(rp, cx + 3, y + 2); Draw(rp, cx + 1, y + 3);
        Move(rp, cx + 3, y + 5); Draw(rp, cx - 2, y + 5);          /* bottom shaft, points left */
        Move(rp, cx - 1, y + 4); Draw(rp, cx - 3, y + 5); Draw(rp, cx - 1, y + 6);
    }
}

static void drawPane(struct Pane *p)
{
    struct RastPort *rp = win->RPort;
    int i;

    int isActive = (p == &panes[g_active]);

    /* path bar - current volume:folder, highlighted on the active pane */
    {
        char pb[88]; int maxc = p->rw / g_cw - 1;
        if (maxc > 87) maxc = 87;
        if (maxc < 1)  maxc = 1;
        strncpy(pb, p->path, maxc); pb[maxc] = '\0';
        SetAPen(rp, isActive ? pFILL : pSHADOW);
        RectFill(rp, p->rx - 1, p->py, p->rx + p->rw, p->py + g_rowH - 1);
        SetAPen(rp, isActive ? pFTXT : pSHINE); SetDrMd(rp, JAM1);
        Move(rp, p->rx + 2, p->py + g_base); Text(rp, pb, strlen(pb));
    }

    /* header bar */
    SetAPen(rp, pSHADOW);
    RectFill(rp, p->rx, p->hy, p->rx + p->rw - 1, p->hy + g_rowH - 1);
    SetAPen(rp, pSHINE); SetDrMd(rp, JAM1);
    Move(rp, p->colName, p->hy + g_base);    Text(rp, "Name", 4);
    Move(rp, p->colSizeBeg, p->hy + g_base); Text(rp, "Size", 4);
    {   /* ascending/descending indicator on the active sort column */
        WORD ax = (p->sortBy == 1) ? p->colSizeBeg : p->colName;
        Move(rp, ax + 5 * g_cw, p->hy + g_base);
        Text(rp, p->sortRev ? (STRPTR)"v" : (STRPTR)"^", 1);
    }

    /* rows */
    for (i = 0; i < p->visRows; i++) {
        int idx = p->top + i;
        WORD ry = p->ry + i * g_rowH;
        BOOL sel = (idx >= 0 && idx == p->sel) ||
                   (idx >= 0 && idx < p->count && p->entries[idx].tagged);   /* tagged rows highlight too */
        SetAPen(rp, sel ? pFILL : pBG);
        RectFill(rp, p->rx, ry, p->rx + p->rw - 1, ry + g_rowH - 1);
        if (idx >= 0 && idx < p->count) {
            struct Entry *e = &p->entries[idx];
            char nb[80];
            int nc = p->nameChars; if (nc > 79) nc = 79;
            strncpy(nb, e->name, nc); nb[nc] = '\0';
            SetDrMd(rp, JAM1);
            SetAPen(rp, sel ? pFTXT : pTEXT);
            Move(rp, p->colName, ry + g_base);                          Text(rp, nb, strlen(nb));
            Move(rp, p->colSizeEnd - strlen(e->sizestr) * g_cw, ry + g_base); Text(rp, e->sizestr, strlen(e->sizestr));
        }
    }
    /* recessed box around the whole pane (path bar + header + list + scrollbar) */
    {
        WORD bx = p->rx - 3, by = p->py - 2;
        WORD ex = p->sbx + p->sbw + 1, ey = p->ry + p->rh + 1;
        SetAPen(rp, pSHADOW);                         /* top + left */
        Move(rp, bx, ey); Draw(rp, bx, by); Draw(rp, ex, by);
        SetAPen(rp, pSHINE);                          /* bottom + right */
        Draw(rp, ex, ey); Draw(rp, bx, ey);
        /* divider under the path bar, separating the two header rows */
        SetAPen(rp, pSHADOW); Move(rp, bx, p->hy - 1); Draw(rp, ex, p->hy - 1);
    }
    drawScrollbar(p);
    /* fill the two empty squares above the scrollbar with Parent + Reload gadgets */
    drawPaneGadget(p->sbx, p->py, p->sbw, g_rowH, 0);   /* Parent (up) over the path row   */
    drawPaneGadget(p->sbx, p->hy, p->sbw, g_rowH, 1);   /* Reload over the Name/Size header */
}

static void syncScroller(struct Pane *p)   /* keep top within range after a rescan */
{
    int maxtop = p->count - p->visRows; if (maxtop < 0) maxtop = 0;
    if (p->top > maxtop) p->top = maxtop;
    if (p->top < 0) p->top = 0;
}

static void fillBG(void)
{
    SetAPen(win->RPort, pBG);
    RectFill(win->RPort, win->BorderLeft, win->BorderTop,
             win->Width - win->BorderRight - 1, win->Height - win->BorderBottom - 1);
}

static void drawTools(void)
{
    struct RastPort *rp = win->RPort;
    int k;
    for (k = 0; k < NBTN; k++) {
        WORD x = g_btnX[k], y = g_btnYr[k], w = g_btnWr[k], h = g_btnH;
        int len = strlen(toolLabels[k]);
        WORD tx = x + (w - len * g_cw) / 2; if (tx < x + 2) tx = x + 2;
        SetAPen(rp, pBG);     RectFill(rp, x, y, x + w - 1, y + h - 1);
        SetAPen(rp, pSHINE);  Move(rp, x, y + h - 1); Draw(rp, x, y); Draw(rp, x + w - 1, y);
        SetAPen(rp, pSHADOW); Move(rp, x + w - 1, y + 1); Draw(rp, x + w - 1, y + h - 1); Draw(rp, x, y + h - 1);
        SetAPen(rp, pTEXT);   SetDrMd(rp, JAM1); Move(rp, tx, y + g_base + 1); Text(rp, toolLabels[k], len);
    }
}

/* compute all geometry from the WINDOW's current inner size (so it reflows on resize) */
static void computeLayout(void)
{
    WORD x0 = win->BorderLeft, y0 = win->BorderTop;
    WORD x1 = win->Width - win->BorderRight, y1 = win->Height - win->BorderBottom;
    WORD innerW = x1 - x0, innerH = y1 - y0;
    WORD margin = 6, scW = 16, btnH = g_rowH + 4;
    WORD topY = y0 + 2;
    WORD listTop = topY + 2 * g_rowH + 2, listBot;
    WORD paneW = (innerW - margin * 3) / 2;
    int px, kk, r, nrows = 1, rcount[8], placed[8];
    WORD rowY[8];
    (void)innerH;
    /* button bank: lay out toolRow[] into stacked rows, row 0 on top */
    for (kk = 0; kk < NBTN; kk++) if (toolRow[kk] + 1 > nrows) nrows = toolRow[kk] + 1;
    for (r = 0; r < nrows; r++) { rcount[r] = 0; placed[r] = 0; }
    for (kk = 0; kk < NBTN; kk++) rcount[toolRow[kk]]++;
    for (r = 0; r < nrows; r++) rowY[r] = y1 - btnH - 2 - (nrows - 1 - r) * (btnH + 2);
    g_btnH = btnH; g_btnTop = rowY[0]; listBot = rowY[0] - 4;
    for (kk = 0; kk < NBTN; kk++) {
        int rr = toolRow[kk], c = rcount[rr];
        WORD w = (innerW - margin * 2 - (c - 1) * 4) / c;
        g_btnX[kk] = x0 + margin + placed[rr] * (w + 4);
        g_btnWr[kk] = w; g_btnYr[kk] = rowY[rr]; placed[rr]++;
    }
    for (px = 0; px < 2; px++) {
        struct Pane *p = &panes[px];
        WORD x = x0 + margin + px * (paneW + margin);
        p->py = topY;  p->hy = topY + g_rowH + 1;
        p->rx = x + 1; p->ry = listTop;
        p->rw = paneW - scW - 3;
        p->rh = listBot - listTop;
        p->sbx = p->rx + p->rw + 2; p->sby = p->ry; p->sbw = scW; p->sbh = p->rh;
        p->visRows = p->rh / g_rowH;
        p->colName    = p->rx + 2;
        p->colSizeEnd = p->rx + p->rw - 2;                   /* size right-aligned at pane edge */
        p->colSizeBeg = p->colSizeEnd - 7 * g_cw;            /* up to 7-digit size */
        p->nameChars  = (p->colSizeBeg - p->colName - 3) / g_cw;  /* name gets all the freed space */
        if (p->nameChars < 4) p->nameChars = 4;
        if (p->count > p->visRows) { if (p->top > p->count - p->visRows) p->top = p->count - p->visRows; }
        else p->top = 0;
        if (p->top < 0) p->top = 0;
    }
}

/* clear + draw everything custom (panes incl. scrollbars + buttons) */
static void drawCustom(void)
{
    fillBG();
    drawPane(&panes[0]); drawPane(&panes[1]); drawTools();
}

static int hitRow(struct Pane *p, WORD mx, WORD my)
{
    if (mx < p->rx || mx >= p->rx + p->rw) return -2;          /* not this pane */
    if (my < p->ry || my >= p->ry + p->rh) return -2;
    return p->top + (my - p->ry) / g_rowH;                    /* row index (may be >= count) */
}

/* show the selected item's full path + size in the window title (so long names
 * that the Name column clips are always readable somewhere) */
static void showSel(struct Pane *p)
{
    static char tb[260];
    if (p->sel >= 0 && p->sel < p->count) {
        struct Entry *e = &p->entries[p->sel];
        char full[256];
        strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)e->name, sizeof full);
        if (e->isdir) fmt(tb, sizeof tb, "%s   [drawer]   %s", full, e->datestr);
        else          fmt(tb, sizeof tb, "%s   -   %ld bytes   -   %s", full, (long)e->size, e->datestr);
        SetWindowTitles(win, (UBYTE *)tb, (UBYTE *)~0L);
    } else {
        SetWindowTitles(win, (UBYTE *)"AmiFM", (UBYTE *)~0L);
    }
}

static void goParent(struct Pane *p)
{
    int len = strlen(p->path);
    if (len == 0 || p->path[len - 1] == ':') return;
    while (len > 0 && p->path[len - 1] != '/' && p->path[len - 1] != ':') len--;
    if (len > 0 && p->path[len - 1] == '/') len--;
    p->path[len] = '\0';
}

/* ---- requesters (self-contained, no reqtools dependency) ---------------- */

static BOOL getString(const char *title, char *buf, int buflen)
{
    struct Window *rw; struct Gadget *gl = NULL, *gg, *sg; struct NewGadget ng;
    BOOL ok = FALSE, done = FALSE; WORD w = 320, h = 64;

    gg = CreateContext(&gl);
    ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi; ng.ng_Flags = 0;
    ng.ng_LeftEdge = 8; ng.ng_TopEdge = 18; ng.ng_Width = w - 16; ng.ng_Height = 14;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = 1;
    gg = sg = CreateGadget(STRING_KIND, gg, &ng, GTST_String, (ULONG)buf, GTST_MaxChars, buflen - 1, TAG_END);
    ng.ng_TopEdge = 40; ng.ng_Width = 80; ng.ng_Height = 14; ng.ng_GadgetText = (STRPTR)"Ok"; ng.ng_GadgetID = 2;
    gg = CreateGadget(BUTTON_KIND, gg, &ng, TAG_END);
    ng.ng_LeftEdge = w - 88; ng.ng_GadgetText = (STRPTR)"Cancel"; ng.ng_GadgetID = 3;
    gg = CreateGadget(BUTTON_KIND, gg, &ng, TAG_END);
    if (!gg) { FreeGadgets(gl); return FALSE; }

    rw = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2, WA_Top, (scr->Height - h) / 2,
        WA_Width, w, WA_Height, h, WA_Title, (ULONG)title, WA_Gadgets, (ULONG)gl,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_PubScreen, (ULONG)scr, TAG_END);
    if (!rw) { FreeGadgets(gl); return FALSE; }
    GT_RefreshWindow(rw, NULL);
    ActivateGadget(sg, rw, NULL);
    while (!done) {
        struct IntuiMessage *m; Wait(1L << rw->UserPort->mp_SigBit);
        while ((m = GT_GetIMsg(rw->UserPort))) {
            ULONG cl = m->Class; struct Gadget *gd = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cl == IDCMP_CLOSEWINDOW) done = TRUE;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(rw); GT_EndRefresh(rw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                if (gd->GadgetID == 3) done = TRUE;
                else { ok = TRUE; done = TRUE; }   /* Ok, or Enter in the string */
            }
        }
    }
    if (ok) {
        STRPTR s = NULL;
        GT_GetGadgetAttrs(sg, rw, NULL, GTST_String, (ULONG)&s, TAG_END);
        if (s) { strncpy(buf, (char *)s, buflen - 1); buf[buflen - 1] = '\0'; }
    }
    CloseWindow(rw); FreeGadgets(gl);
    return (ok && buf[0]);
}

static BOOL confirm(const char *msg, const char *name)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es); es.es_Flags = 0;
    es.es_Title = (STRPTR)"AmiFM"; es.es_TextFormat = (STRPTR)"%s\n%s";
    es.es_GadgetFormat = (STRPTR)"Yes|No";
    return EasyRequest(win, &es, NULL, (ULONG)msg, (ULONG)name) == 1;
}

static void info(const char *msg)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof(es); es.es_Flags = 0;
    es.es_Title = (STRPTR)"AmiFM"; es.es_TextFormat = (STRPTR)"%s";
    es.es_GadgetFormat = (STRPTR)"OK";
    EasyRequest(win, &es, NULL, (ULONG)msg);
}

/* ---- file operations ---------------------------------------------------- */

static struct Pane *active(void) { return &panes[g_active]; }
static struct Pane *other(void)  { return &panes[g_active ^ 1]; }

/* Copy one file. On any failure (read/write error, Ctrl-C) the partial
 * destination is deleted; on success the source's comment, date and
 * protection bits are carried over. */
static BOOL copyFile(const char *src, const char *dst)
{
    struct FileInfoBlock *fib; BPTR lk, in, out; char *buf;
    LONG bufsz = 32768, n = 0; BOOL ok = TRUE, haveInfo = FALSE;
    while (!(buf = AllocVec(bufsz, MEMF_ANY)) && bufsz > 1024) bufsz >>= 1;
    if (!buf) return FALSE;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {       /* source metadata to clone */
        if ((lk = Lock((STRPTR)src, ACCESS_READ))) { haveInfo = Examine(lk, fib); UnLock(lk); }
    }
    if (!(in = Open((STRPTR)src, MODE_OLDFILE))) ok = FALSE;
    else if (!(out = Open((STRPTR)dst, MODE_NEWFILE))) { Close(in); ok = FALSE; }
    else {
        while ((n = Read(in, buf, bufsz)) > 0) {
            if (Write(out, buf, n) != n) { ok = FALSE; break; }
            if (CheckSignal(SIGBREAKF_CTRL_C)) { ok = FALSE; break; }   /* aborted */
        }
        if (n < 0) ok = FALSE;
        Close(in);
        if (!Close(out)) ok = FALSE;                    /* final buffered flush can fail */
        if (!ok) DeleteFile((STRPTR)dst);               /* don't leave a truncated copy */
        else if (haveInfo) {                            /* protection last: it may drop 'w'/'d' */
            if (fib->fib_Comment[0]) SetComment((STRPTR)dst, (STRPTR)fib->fib_Comment);
            SetFileDate((STRPTR)dst, &fib->fib_Date);
            SetProtection((STRPTR)dst, fib->fib_Protection);
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    FreeVec(buf);
    return ok;
}

static void refresh1(struct Pane *p) { scanPane(p); syncScroller(p); drawPane(p); }

/* ---- tagging / multi-select working set --------------------------------- */

struct WItem { char name[108]; BOOL isdir; };

static void clearTags(struct Pane *p)
{
    int i; for (i = 0; i < p->count; i++) p->entries[i].tagged = FALSE;
}

#define IS_DOTDOT(nm) ((nm)[0] == '.' && (nm)[1] == '.' && (nm)[2] == '\0')
#define IS_DOT(nm)    ((nm)[0] == '.' && (nm)[1] == '\0')
#define IS_NAV(nm)    (IS_DOT(nm) || IS_DOTDOT(nm))

/* Snapshot the working set BY NAME (tagged rows, or the cursor row if none
 * tagged) so destructive ops can rescan without index drift. ".." is excluded.
 * Returns the count and an AllocVec'd array in *out (caller FreeVec()s it;
 * NULL when the count is 0). Returns -1 if the array can't be allocated. */
static int gatherWork(struct Pane *p, struct WItem **out)
{
    int n = 0, i; BOOL useSel = FALSE; struct WItem *w;
    *out = NULL;
    for (i = 0; i < p->count; i++)
        if (p->entries[i].tagged && !IS_NAV(p->entries[i].name)) n++;
    if (n == 0) {
        if (p->sel < 0 || p->sel >= p->count || IS_NAV(p->entries[p->sel].name)) return 0;
        n = 1; useSel = TRUE;
    }
    if (!(w = AllocVec(sizeof(struct WItem) * n, MEMF_ANY))) return -1;
    if (useSel) { strcpy(w[0].name, p->entries[p->sel].name); w[0].isdir = p->entries[p->sel].isdir; }
    else {
        int k = 0;
        for (i = 0; i < p->count; i++)
            if (p->entries[i].tagged && !IS_NAV(p->entries[i].name)) {
                strcpy(w[k].name, p->entries[i].name); w[k].isdir = p->entries[i].isdir; k++;
            }
    }
    *out = w;
    return n;
}

static void opSelectAll(void) { struct Pane *p = active(); int i; for (i = 0; i < p->count; i++) p->entries[i].tagged = TRUE; drawPane(p); }
static void opClearTags(void) { clearTags(active()); drawPane(active()); }

static void opSwap(void)   /* swap what the two panes show */
{
    char t[256];
    strcpy(t, panes[0].path);
    strcpy(panes[0].path, panes[1].path);
    strcpy(panes[1].path, t);
    refresh1(&panes[0]); refresh1(&panes[1]);
}

static void opReload(void) { refresh1(active()); }
static void opParent(void) { goParent(active()); refresh1(active()); }

static void opMakeDir(void)
{
    struct Pane *p = active(); char name[108] = ""; char full[384];
    if (getString("New drawer name:", name, sizeof name)) {
        strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)name, sizeof full);
        BPTR d = CreateDir((STRPTR)full);
        if (d) { UnLock(d); refresh1(p); } else info("Could not create drawer.");
    }
}

/* Add Icon: give the selected drawer a system-default folder icon (writes a
 * <drawer>.info via icon.library, so Workbench shows it in icon view). */
static void opAddIcon(void)
{
    struct Pane *p = active(); struct DiskObject *dob; char full[384]; BOOL builtin = FALSE;
    if (!IconBase) { info("icon.library is not available."); return; }
    if (p->sel < 0 || p->sel >= p->count) { info("Select a drawer first."); return; }
    if (!p->entries[p->sel].isdir || IS_NAV(p->entries[p->sel].name)) {
        info("Add Icon is for drawers.\nPick a drawer (<DIR>) first."); return;
    }
    strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)p->entries[p->sel].name, sizeof full);
    if ((dob = GetDiskObject((STRPTR)full))) { FreeDiskObject(dob); info("That drawer already has an icon."); return; }
    dob = GetDefDiskObject(WBDRAWER);          /* the real system-default folder icon */
    if (!dob) {                                /* minimal system: build a folder icon by hand */
        g_drawerDD.dd_NewWindow.LeftEdge = 40; g_drawerDD.dd_NewWindow.TopEdge = 30;
        g_drawerDD.dd_NewWindow.Width = 320; g_drawerDD.dd_NewWindow.Height = 160;
        g_drawerDD.dd_NewWindow.MinWidth = 90; g_drawerDD.dd_NewWindow.MinHeight = 40;
        g_drawerDD.dd_NewWindow.MaxWidth = (UWORD)~0; g_drawerDD.dd_NewWindow.MaxHeight = (UWORD)~0;
        g_drawerDD.dd_NewWindow.Type = WBENCHSCREEN;
        g_drawerDO.do_Magic = WB_DISKMAGIC; g_drawerDO.do_Version = WB_DISKVERSION;
        g_drawerDO.do_Gadget.Width = 18; g_drawerDO.do_Gadget.Height = 14;
        g_drawerDO.do_Gadget.Flags = GFLG_GADGIMAGE; g_drawerDO.do_Gadget.GadgetType = GTYP_BOOLGADGET;
        g_drawerDO.do_Gadget.GadgetRender = (APTR)&g_drawerImg;
        g_drawerDO.do_Type = WBDRAWER; g_drawerDO.do_DrawerData = &g_drawerDD;
        g_drawerDO.do_StackSize = 4096;
        dob = &g_drawerDO; builtin = TRUE;
    }
    dob->do_CurrentX = NO_ICON_POSITION; dob->do_CurrentY = NO_ICON_POSITION;
    if (!PutDiskObject((STRPTR)full, dob)) info("Could not write the icon (.info).");
    else refresh1(p);                          /* the new <drawer>.info now shows in the list */
    if (!builtin) FreeDiskObject(dob);
}

static void opRename(void)
{
    struct Pane *p = active(); char name[108], oldp[384], newp[384];
    if (p->sel < 0 || p->sel >= p->count) { info("Select an item first."); return; }
    if (IS_NAV(p->entries[p->sel].name)) return;
    strncpy(name, p->entries[p->sel].name, sizeof name - 1); name[sizeof name - 1] = '\0';
    if (getString("Rename to:", name, sizeof name)) {
        strcpy(oldp, p->path); AddPart((STRPTR)oldp, (STRPTR)p->entries[p->sel].name, sizeof oldp);
        strcpy(newp, p->path); AddPart((STRPTR)newp, (STRPTR)name, sizeof newp);
        if (Rename((STRPTR)oldp, (STRPTR)newp)) refresh1(p); else info("Rename failed.");
    }
}

static void opDelete(void)
{
    struct Pane *p = active(); struct WItem *w; char full[384], q[80];
    int n = gatherWork(p, &w), i, fail = 0;
    if (n < 0) { info("Out of memory."); return; }
    if (!n) { info("Select or tag items first."); return; }
    if (n == 1) { if (!confirm("Delete this item?", w[0].name)) { FreeVec(w); return; } }
    else { fmt(q, sizeof q, "Delete %d tagged items?", n); if (!confirm(q, "")) { FreeVec(w); return; } }
    for (i = 0; i < n; i++) {
        strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)w[i].name, sizeof full);
        if (!DeleteFile((STRPTR)full)) fail++;
    }
    FreeVec(w);
    clearTags(p); refresh1(p);
    if (fail) { fmt(q, sizeof q, "%d of %d failed (drawer not empty?).", fail, n); info(q); }
}

/* overwrite prompt: 1=Yes, 2=All (don't ask again), 3=Skip, 0=Cancel */
static int overwriteAsk(const char *name)
{
    struct EasyStruct es;
    es.es_StructSize = sizeof es; es.es_Flags = 0;
    es.es_Title = (STRPTR)"AmiFM";
    es.es_TextFormat = (STRPTR)"Overwrite existing file?\n%s";
    es.es_GadgetFormat = (STRPTR)"Yes|All|Skip|Cancel";
    return EasyRequest(win, &es, NULL, (ULONG)name);
}

static void opCopy(void)   /* active pane -> other pane */
{
    struct Pane *s = active(), *d = other(); struct WItem *w;
    char src[384], dst[384], m[80]; int n = gatherWork(s, &w), i;
    int copied = 0, fail = 0, skipdir = 0, skipover = 0;
    BOOL yesall = FALSE, cancelled = FALSE;
    if (n < 0) { info("Out of memory."); return; }
    if (!n) { info("Select or tag items first."); return; }
    if (ci_cmp(s->path, d->path) == 0) {       /* same drawer: copyFile would truncate the file onto itself */
        FreeVec(w); info("Both panes show the same drawer.\nNothing to copy."); return;
    }
    for (i = 0; i < n; i++) {
        if (w[i].isdir) { skipdir++; continue; }     /* recursive drawer copy still TODO */
        strcpy(src, s->path); AddPart((STRPTR)src, (STRPTR)w[i].name, sizeof src);
        strcpy(dst, d->path); AddPart((STRPTR)dst, (STRPTR)w[i].name, sizeof dst);
        if (!yesall) {                               /* confirm before clobbering an existing file */
            BPTR ex = Lock((STRPTR)dst, ACCESS_READ);
            if (ex) {
                int r; UnLock(ex);
                r = overwriteAsk(w[i].name);
                if (r == 0) { cancelled = TRUE; break; }   /* Cancel: stop the whole copy */
                if (r == 3) { skipover++; continue; }      /* Skip this one */
                if (r == 2) yesall = TRUE;                 /* All: stop asking */
            }
        }
        if (copyFile(src, dst)) copied++; else fail++;
    }
    FreeVec(w);
    clearTags(s); refresh1(d); drawPane(s);
    if (cancelled || fail || skipdir || skipover)
        { fmt(m, sizeof m, "%d copied, %d failed, %d skipped, %d drawer(s).",
                copied, fail, skipover, skipdir); info(m); }
}

static void opMove(void)   /* active pane -> other pane (same-volume rename) */
{
    struct Pane *s = active(), *d = other(); struct WItem *w;
    char oldp[384], newp[384], m[80]; int n = gatherWork(s, &w), i, fail = 0;
    if (n < 0) { info("Out of memory."); return; }
    if (!n) { info("Select or tag items first."); return; }
    if (ci_cmp(s->path, d->path) == 0) { FreeVec(w); info("Both panes show the same drawer."); return; }
    for (i = 0; i < n; i++) {
        strcpy(oldp, s->path); AddPart((STRPTR)oldp, (STRPTR)w[i].name, sizeof oldp);
        strcpy(newp, d->path); AddPart((STRPTR)newp, (STRPTR)w[i].name, sizeof newp);
        if (!Rename((STRPTR)oldp, (STRPTR)newp)) fail++;
    }
    FreeVec(w);
    clearTags(s); refresh1(s); refresh1(d);
    if (fail) { fmt(m, sizeof m, "%d of %d failed (cross-volume move comes next).", fail, n); info(m); }
}

/* ---- launch external tools: View (MultiView), Extract (xad), Pack (lha) -- */

static void launchCommand(const char *cmd, BOOL async)
{
    BPTR in  = Open((STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    LONG rc = SystemTags((STRPTR)cmd,
        SYS_Input,  (ULONG)in, SYS_Output, (ULONG)out,
        SYS_Asynch, (ULONG)(async ? TRUE : FALSE), TAG_END);
    if (async) { if (rc != 0) { if (in) Close(in); if (out) Close(out); } }   /* on success System owns them */
    else       { if (in) Close(in); if (out) Close(out); }
}

static void runInDir(const char *dir, const char *cmd, BOOL async)
{
    BPTR nl = Lock((STRPTR)dir, ACCESS_READ), ol;
    if (!nl) { launchCommand(cmd, async); return; }
    ol = CurrentDir(nl);
    launchCommand(cmd, async);
    CurrentDir(ol);
    UnLock(nl);
}

static BOOL hasExt(const char *name, const char *ext)
{
    int nl = strlen(name), el = strlen(ext);
    return (nl >= el) && (ci_cmp(name + nl - el, ext) == 0);
}

static BOOL isArchive(const char *name)
{
    return hasExt(name, ".lha") || hasExt(name, ".lzh") || hasExt(name, ".lzx") ||
           hasExt(name, ".zip") || hasExt(name, ".zoo") || hasExt(name, ".gz")  ||
           hasExt(name, ".tar") || hasExt(name, ".dms") || hasExt(name, ".z");
}

static void opView(void)
{
    struct Pane *p = active(); char cmd[520], full[384];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file first."); return; }
    if (p->entries[p->sel].isdir) { info("That's a drawer (double-click to enter)."); return; }
    strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)p->entries[p->sel].name, sizeof full);
    fmt(cmd, sizeof cmd, "%s \"%s\"", g_set.view, full);   /* Settings > Programs; MultiView by default */
    launchCommand(cmd, TRUE);                        /* async: opens its own window */
}

/* Edit: open the selected file in a GUI text editor - the Edit setting
 * (Settings > Programs) if set, else ENV:EDITOR (e.g. CygnusEd), else the
 * 3.2 bundled SYS:Tools/TextEdit. */
static void opEdit(void)
{
    struct Pane *p = active(); char full[384], ed[120], cmd[520];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file to edit."); return; }
    if (p->entries[p->sel].isdir) { info("That's a drawer, not a file."); return; }
    strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)p->entries[p->sel].name, sizeof full);
    if (g_set.edit[0]) strcpy(ed, g_set.edit);
    else if (GetVar((STRPTR)"EDITOR", (STRPTR)ed, sizeof ed, GVF_GLOBAL_ONLY) <= 0)
        strcpy(ed, "SYS:Tools/TextEdit");
    fmt(cmd, sizeof cmd, "%s \"%s\"", ed, full);
    launchCommand(cmd, TRUE);                         /* async: editor opens its own window */
}

static void opExtract(void)   /* extract tagged archive(s) into the other pane */
{
    struct Pane *s = active(), *d = other(); struct WItem *w;
    char cmd[800], arc[384], m[80]; int n = gatherWork(s, &w), i, got = 0, skip = 0;
    if (n < 0) { info("Out of memory."); return; }
    if (!n) { info("Select or tag an archive first."); return; }
    for (i = 0; i < n; i++) {
        if (w[i].isdir || !isArchive(w[i].name)) { skip++; continue; }
        strcpy(arc, s->path); AddPart((STRPTR)arc, (STRPTR)w[i].name, sizeof arc);
        fmt(cmd, sizeof cmd, "%s \"%s\" DEST \"%s\" OVERWRITE", g_set.extract, arc, d->path);
        launchCommand(cmd, FALSE);                    /* extract into the destination pane */
        got++;
    }
    FreeVec(w);
    clearTags(s); refresh1(d); drawPane(s);
    if (skip) { fmt(m, sizeof m, "%d extracted, %d skipped (not an archive).", got, skip); info(m); }
}

static void opPack(void)
{
    struct Pane *p = active(); char name[80] = "", cmd[420];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file/drawer to pack."); return; }
    if (IS_NAV(p->entries[p->sel].name)) return;
    if (!getString("Archive name (.lha added):", name, sizeof name)) return;
    fmt(cmd, sizeof cmd, "%s a \"%s.lha\" \"%s\"", g_set.pack, name, p->entries[p->sel].name);
    runInDir(p->path, cmd, FALSE);                   /* pack within the active pane */
    refresh1(p);
}

static BOOL pickVolume(char *out, int outlen);   /* fwd */

/* Type-aware "do the thing" on the active pane's selected row: enter a drawer,
 * extract an archive, else view it. Shared by left double-click and right-click.
 * Caller must have set g_active to this pane (opExtract/opView act on g_active). */
static void doSmartAction(struct Pane *p)
{
    int row = p->sel;
    if (row < 0 || row >= p->count) return;
    if (IS_DOT(p->entries[row].name)) {           /* "." -> reload this pane */
        scanPane(p); syncScroller(p);
    } else if (IS_DOTDOT(p->entries[row].name)) { /* ".." -> up a drawer, or picker at root */
        int plen = strlen(p->path);
        if (plen > 0 && p->path[plen - 1] == ':') {   /* at a volume root: open the picker */
            char vol[40];
            if (pickVolume(vol, sizeof vol)) {
                strncpy(p->path, vol, sizeof(p->path) - 1); p->path[sizeof(p->path) - 1] = '\0';
                scanPane(p); syncScroller(p);
            }
        } else { goParent(p); scanPane(p); syncScroller(p); }
    } else if (p->entries[row].isdir) {
        AddPart((STRPTR)p->path, (STRPTR)p->entries[row].name, sizeof(p->path));
        scanPane(p); syncScroller(p);
    } else if (isArchive(p->entries[row].name)) {
        opExtract();
    } else {
        opView();
    }
    drawPane(&panes[0]); drawPane(&panes[1]); showSel(p);
}

/* ---- volume / assign picker --------------------------------------------- */

static int cmpRows(const void *a, const void *b) { return ci_cmp((const char *)a, (const char *)b); }

/* non-filesystem handlers to hide from the picker (can't be browsed as drawers) */
static int isSkipDev(const char *n)
{
    static const char *skip[] = { "CON","RAW","NIL","SER","PAR","PRT","AUX","PIPE",
                                  "CONCLIP","KEYMAP","TCP","USB", NULL };
    int i; for (i = 0; skip[i]; i++) if (ci_cmp(n, skip[i]) == 0) return 1;
    return 0;
}

static int getVolumes(char vols[][40], int max)
{
    struct DosList *e;
    int n = 0;
    /* volumes (disk names) + devices (DH0:/DF0:/RAM:/...) + assigns (C:/S:/LIBS:/...) */
    ULONG flags = LDF_VOLUMES | LDF_DEVICES | LDF_ASSIGNS | LDF_READ;
    e = LockDosList(flags);
    while ((e = NextDosEntry(e, flags)) && n < max) {
        UBYTE *b = (UBYTE *)BADDR(e->dol_Name);
        if (b) {
            int len = b[0]; if (len > 37) len = 37;
            char nm[40];
            strncpy(nm, (char *)(b + 1), len); nm[len] = '\0';
            if (e->dol_Type == DLT_DEVICE && isSkipDev(nm)) continue;  /* hide CON:/SER:/etc. */
            strcpy(vols[n], nm); vols[n][len] = ':'; vols[n][len + 1] = '\0';
            n++;
        }
    }
    UnLockDosList(flags);
    if (n > 1) qsort(vols, n, 40, cmpRows);
    return n;
}

#define PICK_SBW 16
/* generic scrollable list: entries are at base + idx*stride (NUL-terminated strings) */
static void drawListRow(struct Window *vw, char *base, int stride, int n, int top, int idx, BOOL on)
{
    struct RastPort *rp = vw->RPort;
    WORD lx = vw->BorderLeft + 2, ly = vw->BorderTop + 1;
    WORD listBot = vw->Height - vw->BorderBottom - 1;
    WORD sbx = vw->Width - vw->BorderRight - PICK_SBW - 1;
    int  row = idx - top, mc;
    WORD ry;
    if (idx < 0 || idx >= n || row < 0) return;
    ry = ly + row * g_rowH;
    if (ry + g_rowH - 1 > listBot) return;                 /* not currently visible */
    {
        char *s = base + (long)idx * stride; int len = strlen(s);
        mc = (sbx - lx) / g_cw; if (mc < 1) mc = 1; if (len > mc) len = mc;   /* clip to width */
        SetAPen(rp, on ? pFILL : pBG);
        RectFill(rp, vw->BorderLeft + 1, ry, sbx - 1, ry + g_rowH - 1);
        SetAPen(rp, on ? pFTXT : pTEXT); SetDrMd(rp, JAM1);
        Move(rp, lx, ry + g_base); Text(rp, s, len);
    }
}

static void drawList(struct Window *vw, char *base, int stride, int n, int top, int hi)
{
    struct RastPort *rp = vw->RPort;
    WORD ly = vw->BorderTop + 1, listBot = vw->Height - vw->BorderBottom - 1;
    WORD sbx = vw->Width - vw->BorderRight - PICK_SBW - 1, sby = ly, sbh = listBot - ly;
    int  visRows = (listBot - ly) / g_rowH; if (visRows < 1) visRows = 1;
    int  i;
    SetAPen(rp, pBG); RectFill(rp, vw->BorderLeft, ly, sbx - 1, listBot);
    for (i = 0; i < visRows; i++) drawListRow(vw, base, stride, n, top, top + i, (top + i) == hi);
    drawScrollbarAt(rp, sbx, sby, PICK_SBW, sbh, n, visRows, top);
}

/* generic resizable/scrollable chooser; returns chosen index or -1 */
static int chooseList(const char *title, char *base, int stride, int n)
{
    struct Window *vw;
    int chosen = -1, i, hov = -1, top = 0, maxl = 12, rows;
    WORD tH = scr->WBorTop + (scr->Font ? scr->Font->ta_YSize : 8) + 1;
    WORD w, h;
    BOOL done = FALSE;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) { int l = strlen(base + (long)i * stride); if (l > maxl) maxl = l; }
    if (maxl > 76) maxl = 76;                       /* Grep rows are long; the window still fits the screen */
    w = maxl * g_cw + PICK_SBW + 14; if (w < 150) w = 150; if (w > scr->Width - 20) w = scr->Width - 20;
    /* inner height for the rows themselves (the bottom border is taller than WBorBottom
     * with a size gadget, so an outer height came out a row short) */
    rows = (n < 18 ? n : 18);
    while (rows > 1 && tH + rows * g_rowH + 2 + 12 > scr->Height - 10) rows--;
    h = tH + rows * g_rowH + 2 + 12;
    vw = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2, WA_Top, (scr->Height - h) / 2,
        WA_Width, w, WA_InnerHeight, rows * g_rowH + 2, WA_MinWidth, 120, WA_MinHeight, 70,
        WA_MaxWidth, (ULONG)scr->Width, WA_MaxHeight, (ULONG)scr->Height,
        WA_Title, (ULONG)title,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_NEWSIZE,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_SIZEGADGET | WFLG_SIZEBRIGHT |
                  WFLG_ACTIVATE | WFLG_SMART_REFRESH | WFLG_RMBTRAP,
        WA_ReportMouse, TRUE,
        WA_PubScreen, (ULONG)scr, TAG_END);
    if (!vw) return -1;
    SetFont(vw->RPort, font);
    drawList(vw, base, stride, n, top, -1);
    while (!done) {
        struct IntuiMessage *m; Wait(1L << vw->UserPort->mp_SigBit);
        while ((m = (struct IntuiMessage *)GetMsg(vw->UserPort))) {
            ULONG cl = m->Class, code = m->Code; WORD mx = m->MouseX, my = m->MouseY;
            ReplyMsg((struct Message *)m);
            {
                WORD ly = vw->BorderTop + 1, listBot = vw->Height - vw->BorderBottom - 1;
                WORD sbx = vw->Width - vw->BorderRight - PICK_SBW - 1, sby = ly, sbh = listBot - ly;
                int  visRows = (listBot - ly) / g_rowH; if (visRows < 1) visRows = 1;
                int  maxtop = n - visRows; if (maxtop < 0) maxtop = 0;
                if (top > maxtop) top = maxtop;
                if (cl == IDCMP_CLOSEWINDOW) done = TRUE;
                else if (cl == IDCMP_NEWSIZE) { drawList(vw, base, stride, n, top, hov); }
                else if (cl == IDCMP_REFRESHWINDOW) { BeginRefresh(vw); drawList(vw, base, stride, n, top, hov); EndRefresh(vw, TRUE); }
                else if (cl == IDCMP_MOUSEMOVE) {
                    int r = -1;
                    if (mx >= vw->BorderLeft && mx < sbx && my >= ly && my < listBot) {
                        r = top + (my - ly) / g_rowH; if (r >= n) r = -1;
                    }
                    if (r != hov) {
                        int oldhov = hov; hov = r;      /* repaint only the 2 changed rows -> no flicker */
                        if (oldhov >= 0) drawListRow(vw, base, stride, n, top, oldhov, FALSE);
                        if (hov >= 0)    drawListRow(vw, base, stride, n, top, hov, TRUE);
                    }
                }
                else if (cl == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                    if (mx >= sbx && mx < sbx + PICK_SBW && my >= sby && my < sby + sbh) {
                        WORD ah = PICK_SBW, trackTop = sby + ah, trackH = sbh - 2 * ah;
                        if (my < sby + ah)               top--;
                        else if (my >= sby + sbh - ah)   top++;
                        else if (trackH > 0) { int rel = my - trackTop; if (rel < 0) rel = 0; if (rel > trackH) rel = trackH; top = (int)((long)rel * maxtop / trackH); }
                        if (top < 0) top = 0;
                        if (top > maxtop) top = maxtop;
                        drawList(vw, base, stride, n, top, hov);
                    } else if (mx >= vw->BorderLeft && mx < sbx && my >= ly && my < listBot) {
                        int r = top + (my - ly) / g_rowH;
                        if (r >= 0 && r < n) { chosen = r; done = TRUE; }
                    }
                }
            }
        }
    }
    CloseWindow(vw);
    return chosen;
}

static BOOL pickVolume(char *out, int outlen)
{
    char (*vols)[40] = AllocVec(80 * 40, MEMF_ANY);   /* heap: keep it off the small stack */
    int nv, idx;
    if (!vols) { info("Out of memory."); return FALSE; }
    nv = getVolumes(vols, 80);
    idx = (nv > 0) ? chooseList("Pick volume / device / assign", (char *)vols, 40, nv) : -1;
    if (idx >= 0) { strncpy(out, vols[idx], outlen - 1); out[outlen - 1] = '\0'; }
    FreeVec(vols);
    return idx >= 0;
}

/* ---- Find and Grep: AmiFind's and AmiGrep's engines (engine/) ----------- */

#define RES_MAX  1000           /* results kept per search; the search stops there */
#define RES_PATH 256            /* a result's full path */
#define RES_DISP 160            /* Grep: the "file:line: text" row that is listed */

struct Results {
    char *path, *disp;          /* n x RES_PATH, and n x RES_DISP for Grep */
    int   n, cap;
    BOOL  grep, full;           /* full: RES_MAX (or memory) reached, search stopped */
};

static void resFree(struct Results *r)
{
    if (r->path) FreeVec(r->path);
    if (r->disp) FreeVec(r->disp);
    r->path = r->disp = NULL; r->n = r->cap = 0;
}

/* room for one more result: the tables grow on demand (64, 128, ... RES_MAX) */
static BOOL resGrow(struct Results *r)
{
    char *np, *nd = NULL; int nc;
    if (r->n < r->cap) return TRUE;
    if (r->cap >= RES_MAX) { r->full = TRUE; return FALSE; }
    nc = r->cap ? r->cap * 2 : 64; if (nc > RES_MAX) nc = RES_MAX;
    if (!(np = AllocVec((ULONG)nc * RES_PATH, MEMF_ANY))) { r->full = TRUE; return FALSE; }
    if (r->grep && !(nd = AllocVec((ULONG)nc * RES_DISP, MEMF_ANY))) { FreeVec(np); r->full = TRUE; return FALSE; }
    if (r->n) {
        CopyMem(r->path, np, (ULONG)r->n * RES_PATH);
        if (nd) CopyMem(r->disp, nd, (ULONG)r->n * RES_DISP);
    }
    if (r->path) FreeVec(r->path);
    if (r->disp) FreeVec(r->disp);
    r->path = np; r->disp = nd; r->cap = nc;
    return TRUE;
}

/* the search's own small window: live counts and a Stop button (Esc, Ctrl-C too) */
struct Busy {
    struct Window  *w;
    struct Gadget  *gl;
    struct Results *res;
    ULONG          *files, *dirs;   /* the engine's running counts */
    BOOL            stop;
};

static void busyDraw(struct Busy *b)
{
    char t[72]; struct RastPort *rp; WORD x0, y0;
    if (!b->w) return;
    rp = b->w->RPort; x0 = b->w->BorderLeft + 4; y0 = b->w->BorderTop + 3;
    fmt(t, sizeof t, "%d found  %d files  %d drawers", (long)b->res->n, (long)*b->files, (long)*b->dirs);
    SetAPen(rp, pBG); RectFill(rp, x0, y0, b->w->Width - b->w->BorderRight - 5, y0 + g_rowH);
    SetAPen(rp, pTEXT); SetDrMd(rp, JAM1);
    Move(rp, x0 + 4, y0 + g_base); Text(rp, t, strlen(t));
}

static void busyOpen(struct Busy *b, const char *title)
{
    struct NewGadget ng; struct Gadget *g;
    WORD tH = scr->WBorTop + (scr->Font ? scr->Font->ta_YSize : 8) + 1;
    WORD w = 340, h, by = tH + 3 + g_rowH + 6;
    if (w > scr->Width - 8) w = scr->Width - 8;
    h = by + 14 + 6 + scr->WBorBottom;
    b->stop = FALSE; b->w = NULL; b->gl = NULL;
    g = CreateContext(&b->gl);
    ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi; ng.ng_Flags = 0; ng.ng_UserData = NULL;
    ng.ng_LeftEdge = (w - 80) / 2; ng.ng_TopEdge = by; ng.ng_Width = 80; ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Stop"; ng.ng_GadgetID = 1;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(b->gl); b->gl = NULL; return; }   /* no window: Ctrl-C still stops */
    b->w = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2, WA_Top, (scr->Height - h) / 2,
        WA_Width, w, WA_Height, h, WA_Title, (ULONG)title, WA_Gadgets, (ULONG)b->gl,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | BUTTONIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                  WFLG_SMART_REFRESH | WFLG_RMBTRAP,
        WA_PubScreen, (ULONG)scr, TAG_END);
    if (!b->w) { FreeGadgets(b->gl); b->gl = NULL; return; }
    SetFont(b->w->RPort, font);
    GT_RefreshWindow(b->w, NULL);
    busyDraw(b);
}

static void busyClose(struct Busy *b)
{
    if (b->w) CloseWindow(b->w);
    if (b->gl) FreeGadgets(b->gl);
    b->w = NULL; b->gl = NULL;
}

/* the engines' poll hook: TRUE stops the search */
static BOOL busyPoll(APTR user)
{
    struct Busy *b = (struct Busy *)user; struct IntuiMessage *m;
    if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) b->stop = TRUE;
    if (b->w) {
        while ((m = GT_GetIMsg(b->w->UserPort))) {
            ULONG cl = m->Class; UWORD code = m->Code;
            GT_ReplyIMsg(m);
            if (cl == IDCMP_CLOSEWINDOW || cl == IDCMP_GADGETUP) b->stop = TRUE;
            else if (cl == IDCMP_VANILLAKEY && (code == 27 || code == 's' || code == 'S')) b->stop = TRUE;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(b->w); GT_EndRefresh(b->w, TRUE); }
        }
        busyDraw(b);
    }
    return b->stop;
}

/* main window while a search runs: busy pointer; afterwards the clicks and menu
 * picks made on it meanwhile are dropped, not replayed (a queued Delete must
 * not fire), and it is redrawn in case it was resized or uncovered */
static void searchBegin(void)
{
    SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_END);
}

static void searchEnd(void)
{
    struct IntuiMessage *m; BOOL damaged = FALSE;
    SetWindowPointer(win, TAG_END);
    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        if (m->Class == IDCMP_REFRESHWINDOW) damaged = TRUE;
        ReplyMsg((struct Message *)m);
    }
    computeLayout();
    if (damaged) { BeginRefresh(win); drawCustom(); EndRefresh(win, TRUE); }
    else drawCustom();
}

/* show a found file or drawer: its drawer in the pane, the entry selected */
static void jumpTo(struct Pane *p, const char *full)
{
    char dir[256], fn[120]; int i, k = strlen(full), sep = -1;
    for (i = k - 1; i >= 0; i--) if (full[i] == '/' || full[i] == ':') { sep = i; break; }
    if (sep < 0) { strcpy(dir, p->path); strncpy(fn, full, sizeof fn - 1); fn[sizeof fn - 1] = '\0'; }
    else {
        int dl = (full[sep] == ':') ? sep + 1 : sep;     /* keep the colon of "VOL:" */
        if (dl > (int)sizeof dir - 1) dl = sizeof dir - 1;
        memcpy(dir, full, dl); dir[dl] = '\0';
        strncpy(fn, full + sep + 1, sizeof fn - 1); fn[sizeof fn - 1] = '\0';
    }
    strncpy(p->path, dir, sizeof(p->path) - 1); p->path[sizeof(p->path) - 1] = '\0';
    scanPane(p); syncScroller(p);
    for (i = 0; i < p->count; i++)
        if (strcmp(p->entries[i].name, fn) == 0) {
            p->sel = i;
            if (i < p->top || i >= p->top + p->visRows) { p->top = i - p->visRows / 2; if (p->top < 0) p->top = 0; }
            break;
        }
    drawPane(&panes[0]); drawPane(&panes[1]); drawTools(); showSel(p);
}

/* list the results; a click jumps there. Frees the results. */
static void showResults(struct Results *r, const char *what, const char *unit, BOOL stopped)
{
    static char title[80];
    char full[RES_PATH]; int idx;
    fmt(title, sizeof title, "%s: %d %s%s - click to open", what, (long)r->n, unit,
        r->full ? " (the first)" : stopped ? " (stopped)" : "");
    idx = chooseList(title, r->grep ? r->disp : r->path, r->grep ? RES_DISP : RES_PATH, r->n);
    if (idx >= 0) strcpy(full, r->path + (long)idx * RES_PATH);
    resFree(r);
    if (idx >= 0) jumpTo(active(), full);
}

static BOOL findHit(CONST_STRPTR path, BOOL isDir, LONG size, APTR user)
{
    struct Results *r = (struct Results *)user; char *d;
    (void)isDir; (void)size;
    if (!resGrow(r)) return FALSE;
    d = r->path + (long)r->n * RES_PATH;
    strncpy(d, (const char *)path, RES_PATH - 1); d[RES_PATH - 1] = '\0';
    r->n++;
    return TRUE;
}

struct GrepUser { struct Results *r; const char *root; };

static BOOL grepHit(CONST_STRPTR path, ULONG line, CONST_STRPTR text, APTR user)
{
    struct GrepUser *gu = (struct GrepUser *)user; struct Results *r = gu->r;
    const char *rel = (const char *)path, *t = (const char *)text;
    char *d, *e; int rl = strlen(gu->root);
    if (!resGrow(r)) return FALSE;
    d = r->path + (long)r->n * RES_PATH;
    strncpy(d, rel, RES_PATH - 1); d[RES_PATH - 1] = '\0';
    if (rl && strncmp(rel, gu->root, rl) == 0 && rel[rl]) { rel += rl; if (*rel == '/') rel++; }
    d = r->disp + (long)r->n * RES_DISP;                 /* listed relative to the searched drawer */
    fmt(d, RES_DISP, "%s:%d: ", rel, (long)line);
    e = d + strlen(d);
    while (*t == ' ' || *t == '\t') t++;
    while (*t && e < d + RES_DISP - 1) { char c = *t++; *e++ = ((unsigned char)c < 32) ? ' ' : c; }
    *e = '\0';
    r->n++;
    return TRUE;
}

/* the matchers are large: kept off the stack */
static struct Matcher     g_findM;
static struct GrepMatcher g_grepM;
static struct NameFilter  g_grepNF;
static char g_findText[80], g_grepText[120], g_grepFiles[80] = "#?";   /* Files: #? = all */
static BOOL g_grepCase;

static void opFind(void)
{
    struct Pane *p = active(); struct Results r; struct Busy b; struct SearchStats st;
    struct Process *pr; APTR oldwp; LONG err;
    if (!getString("Find (name, or #? pattern):", g_findText, sizeof g_findText)) return;
    if (!matcherInit(&g_findM, g_findText)) { info("Bad search pattern."); return; }
    memset(&r, 0, sizeof r); memset(&st, 0, sizeof st);
    b.res = &r; b.files = &st.filesScanned; b.dirs = &st.dirsScanned;
    searchBegin(); busyOpen(&b, "Find - searching");
    pr = (struct Process *)FindTask(NULL); oldwp = pr->pr_WindowPtr; pr->pr_WindowPtr = (APTR)-1L;
    err = searchTree(p->path, &g_findM, findHit, &r, busyPoll, &b, &st);
    pr->pr_WindowPtr = oldwp;
    busyClose(&b); searchEnd();
    if (err) { resFree(&r); info("Can't read that drawer."); return; }
    if (r.n == 0) { info(st.aborted ? "Stopped before anything was found." : "No matches found."); return; }
    showResults(&r, "Find", "found", st.aborted);
}

/* Grep's requester: the text, which files (a #? pattern; #? or empty = all), match case */
static BOOL getGrepArgs(void)
{
    struct Window *rw; struct Gadget *gl = NULL, *gg, *sText, *sFiles; struct NewGadget ng;
    BOOL ok = FALSE, done = FALSE, cs = g_grepCase;
    WORD tH = scr->WBorTop + (scr->Font ? scr->Font->ta_YSize : 8) + 1;
    WORD w = 340, lx = 64, y = tH + 4, h;
    gg = CreateContext(&gl);
    ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi; ng.ng_UserData = NULL; ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_LeftEdge = lx; ng.ng_TopEdge = y; ng.ng_Width = w - lx - 10; ng.ng_Height = 14;
    ng.ng_GadgetText = (STRPTR)"Text"; ng.ng_GadgetID = 1;
    gg = sText = CreateGadget(STRING_KIND, gg, &ng, GTST_String, (ULONG)g_grepText,
                              GTST_MaxChars, sizeof g_grepText - 1, GA_TabCycle, TRUE, TAG_END);
    ng.ng_TopEdge = y += 18; ng.ng_GadgetText = (STRPTR)"Files"; ng.ng_GadgetID = 2;
    gg = sFiles = CreateGadget(STRING_KIND, gg, &ng, GTST_String, (ULONG)g_grepFiles,
                               GTST_MaxChars, sizeof g_grepFiles - 1, GA_TabCycle, TRUE, TAG_END);
    ng.ng_TopEdge = y += 18; ng.ng_Width = 26; ng.ng_Height = 11; ng.ng_Flags = PLACETEXT_RIGHT;
    ng.ng_GadgetText = (STRPTR)"Match case"; ng.ng_GadgetID = 3;
    gg = CreateGadget(CHECKBOX_KIND, gg, &ng, GTCB_Checked, (ULONG)cs, TAG_END);
    ng.ng_TopEdge = y += 18; ng.ng_LeftEdge = 8; ng.ng_Width = 80; ng.ng_Height = 14; ng.ng_Flags = 0;
    ng.ng_GadgetText = (STRPTR)"Search"; ng.ng_GadgetID = 4;
    gg = CreateGadget(BUTTON_KIND, gg, &ng, TAG_END);
    ng.ng_LeftEdge = w - 88; ng.ng_GadgetText = (STRPTR)"Cancel"; ng.ng_GadgetID = 5;
    gg = CreateGadget(BUTTON_KIND, gg, &ng, TAG_END);
    if (!gg) { FreeGadgets(gl); return FALSE; }
    h = y + 14 + 6 + scr->WBorBottom;

    rw = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2, WA_Top, (scr->Height - h) / 2,
        WA_Width, w, WA_Height, h, WA_Title, (ULONG)"Grep - search inside files",
        WA_Gadgets, (ULONG)gl,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP | CHECKBOXIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_PubScreen, (ULONG)scr, TAG_END);
    if (!rw) { FreeGadgets(gl); return FALSE; }
    GT_RefreshWindow(rw, NULL);
    ActivateGadget(sText, rw, NULL);
    while (!done) {
        struct IntuiMessage *m; Wait(1L << rw->UserPort->mp_SigBit);
        while ((m = GT_GetIMsg(rw->UserPort))) {
            ULONG cl = m->Class; UWORD code = m->Code; struct Gadget *gd = (struct Gadget *)m->IAddress;
            GT_ReplyIMsg(m);
            if (cl == IDCMP_CLOSEWINDOW) done = TRUE;
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(rw); GT_EndRefresh(rw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                switch (gd->GadgetID) {
                    case 1: case 2: if (code != 9) { ok = TRUE; done = TRUE; } break;  /* Enter; Tab moves on */
                    case 3: cs = code ? TRUE : FALSE; break;
                    case 4: ok = TRUE; done = TRUE; break;
                    case 5: done = TRUE; break;
                }
            }
        }
    }
    if (ok) {
        STRPTR s = NULL;
        GT_GetGadgetAttrs(sText, rw, NULL, GTST_String, (ULONG)&s, TAG_END);
        if (s) { strncpy(g_grepText, (char *)s, sizeof g_grepText - 1); g_grepText[sizeof g_grepText - 1] = '\0'; }
        s = NULL;
        GT_GetGadgetAttrs(sFiles, rw, NULL, GTST_String, (ULONG)&s, TAG_END);
        if (s) { strncpy(g_grepFiles, (char *)s, sizeof g_grepFiles - 1); g_grepFiles[sizeof g_grepFiles - 1] = '\0'; }
        g_grepCase = cs;
    }
    CloseWindow(rw); FreeGadgets(gl);
    return (ok && g_grepText[0]);
}

static void opGrep(void)
{
    struct Pane *p = active(); struct Results r; struct Busy b; struct GrepStats st; struct GrepUser gu;
    struct Process *pr; APTR oldwp; LONG err;
    if (!getGrepArgs()) return;
    if (!grepMatcherInit(&g_grepM, g_grepText, g_grepCase)) { info("Bad search text."); return; }
    if (!nameFilterInit(&g_grepNF, g_grepFiles)) { info("Bad Files pattern."); return; }
    memset(&r, 0, sizeof r); memset(&st, 0, sizeof st);
    r.grep = TRUE;
    gu.r = &r; gu.root = p->path;
    b.res = &r; b.files = &st.filesScanned; b.dirs = &st.dirsScanned;
    searchBegin(); busyOpen(&b, "Grep - searching");
    pr = (struct Process *)FindTask(NULL); oldwp = pr->pr_WindowPtr; pr->pr_WindowPtr = (APTR)-1L;
    err = grepTree(p->path, &g_grepM, &g_grepNF, grepHit, &gu, busyPoll, &b, &st);
    pr->pr_WindowPtr = oldwp;
    busyClose(&b); searchEnd();
    if (err) { resFree(&r); info("Can't read that drawer."); return; }
    if (r.n == 0) { info(st.aborted ? "Stopped before anything was found." : "No lines found."); return; }
    showResults(&r, "Grep", "lines", st.aborted);
}

/* ---- right-button menu, window open, iconify --------------------------- */

/* every item carries its MN_* id as user data (GTMENUITEM_USERDATA), so the
 * dispatch in doMenu() does not depend on item positions */
enum {
    MN_NONE = 0,
    MN_RELOAD, MN_ICONIFY, MN_ABOUT, MN_QUIT,                                   /* AmiFM */
    MN_VIEW, MN_EDIT, MN_COPY, MN_MOVE, MN_RENAME, MN_DELETE, MN_ADDICON,       /* Selection */
    MN_EXTRACT, MN_PACK,
    MN_PARENT, MN_MAKEDIR, MN_FIND, MN_GREP, MN_SWAP, MN_SELALL, MN_CLEARTAGS,  /* Pane */
    MN_SET_DRAWERS, MN_SET_PROGRAMS, MN_SNAPSHOT, MN_UNSNAPSHOT                  /* Settings */
};

static struct NewMenu g_newmenu[] = {
    { NM_TITLE, (STRPTR)"AmiFM",            0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Reload",           (STRPTR)"R", 0, 0, (APTR)MN_RELOAD },
    { NM_ITEM,  (STRPTR)"Iconify",          (STRPTR)"I", 0, 0, (APTR)MN_ICONIFY },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"About...",         (STRPTR)"?", 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Quit",             (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_TITLE, (STRPTR)"Selection",        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"View",             (STRPTR)"V", 0, 0, (APTR)MN_VIEW },
    { NM_ITEM,  (STRPTR)"Edit",             (STRPTR)"E", 0, 0, (APTR)MN_EDIT },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Copy",             (STRPTR)"C", 0, 0, (APTR)MN_COPY },
    { NM_ITEM,  (STRPTR)"Move",             (STRPTR)"M", 0, 0, (APTR)MN_MOVE },
    { NM_ITEM,  (STRPTR)"Rename",           (STRPTR)"N", 0, 0, (APTR)MN_RENAME },
    { NM_ITEM,  (STRPTR)"Delete",           (STRPTR)"D", 0, 0, (APTR)MN_DELETE },
    { NM_ITEM,  (STRPTR)"Add Icon",         0,           0, 0, (APTR)MN_ADDICON },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Extract",          (STRPTR)"X", 0, 0, (APTR)MN_EXTRACT },
    { NM_ITEM,  (STRPTR)"Pack",             (STRPTR)"K", 0, 0, (APTR)MN_PACK },
    { NM_TITLE, (STRPTR)"Pane",             0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Parent",           (STRPTR)"P", 0, 0, (APTR)MN_PARENT },
    { NM_ITEM,  (STRPTR)"Make Drawer...",   0,           0, 0, (APTR)MN_MAKEDIR },
    { NM_ITEM,  (STRPTR)"Find...",          (STRPTR)"F", 0, 0, (APTR)MN_FIND },
    { NM_ITEM,  (STRPTR)"Grep...",          (STRPTR)"G", 0, 0, (APTR)MN_GREP },
    { NM_ITEM,  (STRPTR)"Swap Panes",       (STRPTR)"S", 0, 0, (APTR)MN_SWAP },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Select All",       (STRPTR)"A", 0, 0, (APTR)MN_SELALL },
    { NM_ITEM,  (STRPTR)"Clear Tags",       0,           0, 0, (APTR)MN_CLEARTAGS },
    { NM_TITLE, (STRPTR)"Settings",         0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Start drawers...", 0,           0, 0, (APTR)MN_SET_DRAWERS },
    { NM_ITEM,  (STRPTR)"Programs...",      0,           0, 0, (APTR)MN_SET_PROGRAMS },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Snapshot window",  0,           0, 0, (APTR)MN_SNAPSHOT },
    { NM_ITEM,  (STRPTR)"Unsnapshot window", 0,          0, 0, (APTR)MN_UNSNAPSHOT },
    { NM_END,   0,                          0,           0, 0, (APTR)0 }
};

/* ---- Settings pages (amigui frame: Save / Use / Defaults / Cancel) ------
 * Laid out from Topaz 8, the font AmiFM's gadgets use everywhere, so both
 * pages fit a 640x200 NTSC screen. */

#define SP_MAX        4
#define ID_SP_CURRENT 20               /* "Current panes" (Start drawers page) */

struct SetPage {
    const char *title, *hint;
    int n;
    const char *label[SP_MAX];
    char       *val[SP_MAX];           /* into g_set */
    int         size[SP_MAX];
    const char *def[SP_MAX];           /* into a default Settings */
    BOOL        current;               /* fields 0/1 are the panes' drawers */
};

static void spGadget(AmgPage *pg, int id, UWORD code, void *ctx)
{
    struct SetPage *sp = (struct SetPage *)ctx; int i;
    (void)code;
    if (id == AMG_ID_DEFAULTS)
        for (i = 0; i < sp->n; i++)
            GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTST_String, (ULONG)sp->def[i], TAG_END);
    else if (id == ID_SP_CURRENT && sp->current)
        for (i = 0; i < 2; i++)
            GT_SetGadgetAttrs(pg->g[i], pg->win, NULL, GTST_String, (ULONG)panes[i].path, TAG_END);
}

static void runSetPage(struct SetPage *sp)
{
    AmgPage pg; struct NewGadget ng; struct Gadget *gad;
    int i, lw = 0, w, h, bh, rowh, y, r = AMG_CANCEL;
    {   /* measure the labels in Topaz 8 before the window opens */
        struct TextFont *f = OpenFont(&topaz8); struct RastPort rp;
        if (!f) return;
        InitRastPort(&rp); SetFont(&rp, f);
        for (i = 0; i < sp->n; i++) {
            int l = TextLength(&rp, (STRPTR)sp->label[i], strlen(sp->label[i]));
            if (l > lw) lw = l;
        }
        bh = f->tf_YSize + 6;
        CloseFont(f);
    }
    lw += 8; rowh = bh + 4;
    w = 2 * 8 + lw + 340;
    if (w < amg_min_width(&topaz8)) w = amg_min_width(&topaz8);
    if (w > scr->Width - 24) w = scr->Width - 24;
    h = 4 + sp->n * rowh + rowh + (sp->current ? rowh : 0) + 6 + bh + 4;
    if (!amg_page_open(&pg, scr, vi, &topaz8, win, sp->title, w, h)) return;
    gad = CreateContext(&pg.glist);
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi; ng.ng_TextAttr = &topaz8;
    ng.ng_LeftEdge = pg.win->BorderLeft + 8 + lw; ng.ng_Width = w - 2 * 8 - lw;
    ng.ng_Height = bh; ng.ng_Flags = PLACETEXT_LEFT;
    y = 4;
    for (i = 0; i < sp->n; i++) {
        ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
        ng.ng_GadgetText = (STRPTR)sp->label[i]; ng.ng_GadgetID = 1 + i;
        pg.g[i] = gad = CreateGadget(STRING_KIND, gad, &ng, GTST_String, (ULONG)sp->val[i],
                                     GTST_MaxChars, sp->size[i] - 1, TAG_END);
    }
    ng.ng_LeftEdge = pg.win->BorderLeft + 8; ng.ng_Width = w - 2 * 8;   /* the hint line */
    ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)sp->hint, GTTX_Clipped, TRUE, TAG_END);
    if (sp->current) {
        ng.ng_TopEdge = pg.win->BorderTop + y; y += rowh;
        ng.ng_Width = amg_tlen(&pg, "Current panes") + 24; ng.ng_Flags = PLACETEXT_IN;
        ng.ng_GadgetText = (STRPTR)"Current panes"; ng.ng_GadgetID = ID_SP_CURRENT;
        gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    }
    gad = amg_buttons(&pg, gad, y + 6, w, 1);
    if (gad) {
        amg_page_show(&pg);
        r = amg_page_run(&pg, spGadget, sp);
        if (r != AMG_CANCEL)
            for (i = 0; i < sp->n; i++) {
                const char *s = amg_string(pg.g[i]);
                if (!*s && sp->def[i][0]) s = sp->def[i];    /* empty = the default (Edit: ENV:EDITOR) */
                setStr(sp->val[i], sp->size[i], s);
            }
    }
    amg_page_close(&pg);
    if (r != AMG_CANCEL && !setsWrite(&g_set, r == AMG_SAVE))
        info(r == AMG_SAVE ? "Could not write ENV:AmiFM.prefs / ENVARC:AmiFM.prefs."
                           : "Could not write ENV:AmiFM.prefs.");
}

static void settingsPage(int which)
{
    static struct Settings def;
    struct SetPage sp;
    setsDefault(&def);
    memset(&sp, 0, sizeof sp);
    if (which == MN_SET_DRAWERS) {
        sp.title = "AmiFM: Start drawers"; sp.hint = "Used the next time AmiFM starts.";
        sp.n = 2; sp.current = TRUE;
        sp.label[0] = "Left pane";  sp.val[0] = g_set.left;  sp.size[0] = sizeof g_set.left;  sp.def[0] = def.left;
        sp.label[1] = "Right pane"; sp.val[1] = g_set.right; sp.size[1] = sizeof g_set.right; sp.def[1] = def.right;
    } else {
        sp.title = "AmiFM: Programs"; sp.hint = "Edit empty = ENV:EDITOR, else SYS:Tools/TextEdit.";
        sp.n = 4;
        sp.label[0] = "View";    sp.val[0] = g_set.view;    sp.size[0] = sizeof g_set.view;    sp.def[0] = def.view;
        sp.label[1] = "Edit";    sp.val[1] = g_set.edit;    sp.size[1] = sizeof g_set.edit;    sp.def[1] = def.edit;
        sp.label[2] = "Extract"; sp.val[2] = g_set.extract; sp.size[2] = sizeof g_set.extract; sp.def[2] = def.extract;
        sp.label[3] = "Pack";    sp.val[3] = g_set.pack;    sp.size[3] = sizeof g_set.pack;    sp.def[3] = def.pack;
    }
    searchBegin();                     /* busy main window; clicks made meanwhile are dropped */
    runSetPage(&sp);
    searchEnd();
}

static void buildMenu(void)
{
    /* classic menus: items use COMPLEMENT highlighting (always high-contrast/
     * readable on any palette) instead of NewLook's coloured bar */
    g_menu = CreateMenus(g_newmenu, TAG_END);
    if (g_menu && !LayoutMenus(g_menu, vi, GTMN_NewLookMenus, FALSE, TAG_END)) {
        FreeMenus(g_menu); g_menu = NULL;
    }
}

static void iconify(void);   /* fwd */

static BOOL openMainWin(void)
{
    /* the snapshot place and size (Settings > Snapshot window), kept on the
     * screen; else a window that fits one monitor: a wide RTG Workbench
     * (1600x600 over two monitors) used to get a window across both */
    LONG x = 30, y = scr->BarHeight + 6, w = scr->Width - 90, h = scr->Height - scr->BarHeight - 56;
    if (w > 720) w = 720;
    if (g_set.winw > 0) {
        x = g_set.winx; y = g_set.winy; w = g_set.winw; h = g_set.winh;
        if (w > scr->Width) w = scr->Width;
        if (h > scr->Height) h = scr->Height;
        if (x + w > scr->Width) x = scr->Width - w;
        if (y + h > scr->Height) y = scr->Height - h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    }
    if (w < 340) w = 340;
    if (h < 130) h = 130;
    win = OpenWindowTags(NULL,
        WA_Left, (ULONG)x, WA_Top, (ULONG)y,
        WA_Width,  (ULONG)w,
        WA_Height, (ULONG)h,
        WA_MinWidth, 340, WA_MinHeight, 130,
        WA_MaxWidth, (ULONG)scr->Width, WA_MaxHeight, (ULONG)scr->Height,
        WA_Title, (ULONG)"AmiFM",
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_MOUSEBUTTONS | IDCMP_NEWSIZE | IDCMP_MENUPICK,
        WA_Flags, WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_SIZEGADGET |
                  WFLG_SIZEBRIGHT | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_PubScreen, (ULONG)scr,
        TAG_END);
    if (!win) return FALSE;
    SetFont(win->RPort, font);
    if (g_menu) SetMenuStrip(win, g_menu);        /* right-button menu */
    computeLayout();
    drawCustom();
    return TRUE;
}

/* make sure PROGDIR:AmiFM.info exists so AmiFM has a Workbench icon (and the
 * iconify AppIcon uses it); create it from the built-in image if missing. */
static void ensureProgramIcon(void)
{
    struct DiskObject *dob;
    if (!IconBase) return;
    if ((dob = GetDiskObject((STRPTR)"PROGDIR:AmiFM"))) { FreeDiskObject(dob); return; }
    g_appIconDO.do_Magic = WB_DISKMAGIC; g_appIconDO.do_Version = WB_DISKVERSION;
    g_appIconDO.do_Gadget.Width = 16; g_appIconDO.do_Gadget.Height = 16;
    g_appIconDO.do_Gadget.Flags = GFLG_GADGIMAGE; g_appIconDO.do_Gadget.GadgetType = GTYP_BOOLGADGET;
    g_appIconDO.do_Gadget.GadgetRender = (APTR)&g_appIconImg; g_appIconDO.do_Type = WBTOOL;
    g_appIconDO.do_CurrentX = NO_ICON_POSITION; g_appIconDO.do_CurrentY = NO_ICON_POSITION;
    g_appIconDO.do_StackSize = 4096;
    PutDiskObject((STRPTR)"PROGDIR:AmiFM", &g_appIconDO);
}

/* iconify: close the window, drop AmiFM's icon on the Workbench as an AppIcon,
 * wait for a double-click, then reopen. Uses PROGDIR:AmiFM.info if present. */
static void iconify(void)
{
    struct AppIcon *ai; struct MsgPort *port; struct DiskObject *dob; BOOL freedob = FALSE, wake = FALSE;
    if (!WorkbenchBase) { info("Iconify needs workbench.library v37+."); return; }
    if (!(port = CreateMsgPort())) return;
    dob = IconBase ? GetDiskObject((STRPTR)"PROGDIR:AmiFM") : NULL;
    if (dob) freedob = TRUE;
    else {                                        /* fall back to the built-in image */
        g_appIconDO.do_Magic = WB_DISKMAGIC; g_appIconDO.do_Version = WB_DISKVERSION;
        g_appIconDO.do_Gadget.Width = 16; g_appIconDO.do_Gadget.Height = 16;
        g_appIconDO.do_Gadget.Flags = GFLG_GADGIMAGE; g_appIconDO.do_Gadget.GadgetType = GTYP_BOOLGADGET;
        g_appIconDO.do_Gadget.GadgetRender = (APTR)&g_appIconImg; g_appIconDO.do_Type = WBTOOL;
        g_appIconDO.do_StackSize = 4096;
        dob = &g_appIconDO;
    }
    dob->do_CurrentX = NO_ICON_POSITION; dob->do_CurrentY = NO_ICON_POSITION;
    ai = AddAppIconA(0L, 0L, (STRPTR)"AmiFM", port, (BPTR)0, dob, NULL);
    if (!ai) { if (freedob) FreeDiskObject(dob); DeleteMsgPort(port); info("Iconify: AddAppIcon failed."); return; }
    if (g_menu) ClearMenuStrip(win);
    CloseWindow(win); win = NULL;
    while (!wake) {
        struct Message *am;
        WaitPort(port);
        while ((am = GetMsg(port))) { ReplyMsg(am); wake = TRUE; }   /* double-clicked -> reopen */
    }
    RemoveAppIcon(ai);
    if (freedob) FreeDiskObject(dob);
    DeleteMsgPort(port);
    if (!openMainWin())            /* low memory: leave win NULL, main loop exits cleanly */
        info("AmiFM: could not reopen its window.");
}

/* one menu pick (by its MN_* user data); TRUE = quit */
static BOOL doMenu(ULONG id)
{
    switch (id) {
        case MN_RELOAD:    opReload(); break;
        case MN_ICONIFY:   iconify(); return FALSE;      /* the window may be gone */
        case MN_ABOUT:     amg_about(win, "AmiFM", AMIFM_VERSION,
                               "A two-pane file manager for AmigaOS 3.x.\n" AMIFM_LICENCE); break;
        case MN_QUIT:      return TRUE;
        case MN_VIEW:      opView(); break;
        case MN_EDIT:      opEdit(); break;
        case MN_COPY:      opCopy(); break;
        case MN_MOVE:      opMove(); break;
        case MN_RENAME:    opRename(); break;
        case MN_DELETE:    opDelete(); break;
        case MN_ADDICON:   opAddIcon(); break;
        case MN_EXTRACT:   opExtract(); break;
        case MN_PACK:      opPack(); break;
        case MN_PARENT:    opParent(); break;
        case MN_MAKEDIR:   opMakeDir(); break;
        case MN_FIND:      opFind(); break;
        case MN_GREP:      opGrep(); break;
        case MN_SWAP:      opSwap(); break;
        case MN_SELALL:    opSelectAll(); break;
        case MN_CLEARTAGS: opClearTags(); break;
        case MN_SET_DRAWERS:
        case MN_SET_PROGRAMS: settingsPage((int)id); break;
        case MN_SNAPSHOT: {   /* the window's place and size from now on: only the WINDOW
                               * line is saved, settings that were only Used stay so */
            char v[48];
            g_set.winx = win->LeftEdge; g_set.winy = win->TopEdge;
            g_set.winw = win->Width;    g_set.winh = win->Height;
            fmt(v, sizeof v, "%d,%d,%d,%d", g_set.winx, g_set.winy, g_set.winw, g_set.winh);
            info(amg_env_setkey(PREFS_NAME, "WINDOW", v) ? "Window place and size saved."
                 : "Could not write ENV:AmiFM.prefs / ENVARC:AmiFM.prefs.");
            break;
        }
        case MN_UNSNAPSHOT:   /* back to the default place and size at the next start */
            g_set.winw = 0;
            info(amg_env_setkey(PREFS_NAME, "WINDOW", NULL)
                 ? "The window opens at its default place and size again."
                 : "Could not write ENV:AmiFM.prefs / ENVARC:AmiFM.prefs.");
            break;
        default: return FALSE;
    }
    if (win) showSel(active());        /* ops may have cleared the selection */
    return FALSE;
}

int main(void)
{
    BOOL done = FALSE;
    int px;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    GadToolsBase  = OpenLibrary("gadtools.library", 39);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) { PutStr((STRPTR)"AmiFM: need v39 libs\n"); goto cl_libs; }
    WorkbenchBase = OpenLibrary("workbench.library", 37);   /* optional: iconify */
    IconBase      = OpenLibrary("icon.library", 37);

    scr = LockPubScreen(NULL);
    if (!scr) goto cl_libs;
    dri = GetScreenDrawInfo(scr);
    vi  = GetVisualInfo(scr, TAG_END);
    if (!dri || !vi) goto cl_scr;
    font = OpenFont(&topaz8);
    if (!font) goto cl_scr;

    pTEXT = dri->dri_Pens[TEXTPEN]; pBG = dri->dri_Pens[BACKGROUNDPEN];
    pFILL = dri->dri_Pens[FILLPEN]; pFTXT = dri->dri_Pens[FILLTEXTPEN];
    pSHADOW = dri->dri_Pens[SHADOWPEN]; pSHINE = dri->dri_Pens[SHINEPEN];
    g_cw = font->tf_XSize; g_rowH = font->tf_YSize + 1; g_base = font->tf_Baseline + 1;

    /* start drawers: the settings (AmiFM.prefs over the defaults); one that
     * can't be locked now (volume not mounted, drawer gone) falls back */
    setsLoad(&g_set);
    for (px = 0; px < 2; px++) {
        const char *want = px ? g_set.right : g_set.left;
        struct Process *pr = (struct Process *)FindTask(NULL);
        APTR oldwp = pr->pr_WindowPtr; BPTR lk;
        pr->pr_WindowPtr = (APTR)-1L;              /* no "insert volume" requester */
        lk = Lock((STRPTR)want, ACCESS_READ);
        pr->pr_WindowPtr = oldwp;
        if (lk) UnLock(lk); else want = px ? "RAM:" : "SYS:";
        setStr(panes[px].path, sizeof panes[px].path, want);
    }
    panes[0].lastrow = panes[1].lastrow = -1;
    scanPane(&panes[0]); scanPane(&panes[1]);

    ensureProgramIcon();                          /* create PROGDIR:AmiFM.info if missing */
    buildMenu();                                  /* right-button menu */
    if (!openMainWin()) { PutStr((STRPTR)"AmiFM: no window\n"); goto cl_font; }

    while (!done) {
        struct IntuiMessage *imsg;
        if (!win) break;            /* window couldn't be reopened after iconify */
        Wait(1L << win->UserPort->mp_SigBit);
        while ((imsg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = imsg->Class, code = imsg->Code;
            WORD mx = imsg->MouseX, my = imsg->MouseY;
            ULONG sec = imsg->Seconds, mic = imsg->Micros;
            UWORD qual = imsg->Qualifier;
            ReplyMsg((struct Message *)imsg);

            if (cls == IDCMP_CLOSEWINDOW) {
                done = TRUE;
            } else if (cls == IDCMP_MENUPICK) {
                UWORD mnum = code;
                while (mnum != MENUNULL && !done) {
                    struct MenuItem *mi = ItemAddress(g_menu, mnum);
                    if (!mi) break;
                    if (doMenu((ULONG)GTMENUITEM_USERDATA(mi))) done = TRUE;
                    if (!win) break;                     /* iconify couldn't reopen the window */
                    mnum = mi->NextSelect;
                }
                if (!win) break;                         /* don't read a closed window's port */
            } else if (cls == IDCMP_REFRESHWINDOW) {
                BeginRefresh(win); drawCustom(); EndRefresh(win, TRUE);
            } else if (cls == IDCMP_NEWSIZE) {
                computeLayout(); drawCustom();
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                if (my >= g_btnTop) {       /* tool button area (two rows) */
                    int k;
                    for (k = 0; k < NBTN; k++)
                        if (mx >= g_btnX[k] && mx < g_btnX[k] + g_btnWr[k] &&
                            my >= g_btnYr[k] && my < g_btnYr[k] + g_btnH) {
                            switch (k) {
                                case 0: opView();    break; case 1: opEdit();    break;
                                case 2: opCopy();    break; case 3: opMove();    break;
                                case 4: opRename();  break; case 5: opDelete();  break;
                                case 6: opMakeDir(); break; case 7: opAddIcon(); break;
                                case 8: opExtract(); break; case 9: opPack();    break;
                                case 10: opFind();   break; case 11: opGrep();   break;
                                case 12: opSwap();   break;
                                case 13: opSelectAll(); break; case 14: opClearTags(); break; }
                            showSel(active());     /* refresh title (ops may have cleared the selection) */
                            break;
                        }
                } else for (px = 0; px < 2; px++) {
                    struct Pane *p = &panes[px];
                    int row;
                    /* path-bar click -> focus pane + volume picker */
                    if (mx >= p->rx && mx < p->rx + p->rw && my >= p->py && my < p->py + g_rowH) {
                        char vol[40];
                        g_active = px;
                        if (pickVolume(vol, sizeof vol)) {
                            strncpy(p->path, vol, sizeof(p->path) - 1); p->path[sizeof(p->path) - 1] = '\0';
                            scanPane(p); syncScroller(p);
                        }
                        drawPane(&panes[0]); drawPane(&panes[1]); drawTools(); showSel(p);
                        break;
                    }
                    /* header click -> set or toggle the sort column */
                    if (mx >= p->rx && mx < p->rx + p->rw && my >= p->hy && my < p->hy + g_rowH) {
                        int col = (mx >= p->colSizeBeg) ? 1 : 0;
                        g_active = px;
                        if (p->sortBy == col) p->sortRev = !p->sortRev;
                        else { p->sortBy = col; p->sortRev = 0; }
                        sortPane(p); drawPane(&panes[0]); drawPane(&panes[1]); showSel(p);
                        break;
                    }
                    /* Parent/Reload gadgets above the scrollbar (path row + header row) */
                    if (mx >= p->sbx && mx < p->sbx + p->sbw) {
                        if (my >= p->py && my < p->py + g_rowH) {
                            g_active = px; opParent();
                            drawPane(&panes[0]); drawPane(&panes[1]); drawTools(); showSel(p); break;
                        }
                        if (my >= p->hy && my < p->hy + g_rowH) {
                            g_active = px; opReload();
                            drawPane(&panes[0]); drawPane(&panes[1]); drawTools(); showSel(p); break;
                        }
                    }
                    /* scrollbar click -> up/down arrow or position in track */
                    if (mx >= p->sbx && mx < p->sbx + p->sbw &&
                        my >= p->sby && my < p->sby + p->sbh) {
                        WORD ah = p->sbw, trackTop = p->sby + ah, trackH = p->sbh - 2 * ah;
                        int maxtop = p->count - p->visRows; if (maxtop < 0) maxtop = 0;
                        g_active = px;
                        if (my < p->sby + ah)               p->top--;
                        else if (my >= p->sby + p->sbh - ah) p->top++;
                        else if (trackH > 0) {
                            int rel = my - trackTop; if (rel < 0) rel = 0; if (rel > trackH) rel = trackH;
                            p->top = (int)((long)rel * maxtop / trackH);
                        }
                        if (p->top < 0) p->top = 0;
                        if (p->top > maxtop) p->top = maxtop;
                        drawPane(p);
                        break;
                    }
                    row = hitRow(p, mx, my);
                    if (row == -2) continue;
                    if (row >= 0 && row < p->count) {
                        g_active = px;
                        if (row == p->lastrow && DoubleClick(p->lastsec, p->lastmic, sec, mic)) {
                            p->sel = row;
                            doSmartAction(p);
                            p->lastrow = -1;
                        } else if (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) {
                            p->entries[row].tagged ^= 1;   /* shift-click toggles a tag */
                            p->sel = row;
                            drawPane(&panes[0]); drawPane(&panes[1]); showSel(p);
                            p->lastrow = row; p->lastsec = sec; p->lastmic = mic;
                        } else {
                            clearTags(p);                  /* plain click = single select */
                            p->sel = (p->sel == row) ? -1 : row;  /* click again un-highlights */
                            drawPane(&panes[0]); drawPane(&panes[1]); showSel(p);
                            p->lastrow = row; p->lastsec = sec; p->lastmic = mic;
                        }
                    }
                    break;
                }
            }
        }
    }

    if (win) { if (g_menu) ClearMenuStrip(win); CloseWindow(win); }
cl_font:
    if (g_menu) FreeMenus(g_menu);
    CloseFont(font);
cl_scr:
    if (vi)  FreeVisualInfo(vi);
    if (dri) FreeScreenDrawInfo(scr, dri);
    UnlockPubScreen(NULL, scr);
    freeEntries(&panes[0]); freeEntries(&panes[1]);
cl_libs:
    if (IconBase)      CloseLibrary(IconBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (GadToolsBase)  CloseLibrary(GadToolsBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
