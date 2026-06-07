/*
 * AmiFM - dual-pane file manager for AmigaOS 3.x  [custom-lister build]
 *
 * Custom-drawn listers: rows are rendered by hand, so columns are pinned to
 * exact pixels (Name | Size | Date) with headers, right-aligned size, and no
 * clipping. Custom scrollbars per pane handle long lists. Left-click selects;
 * left double-click does the type-aware action (enter drawer / extract archive
 * / view file). The RIGHT button opens the menu strip (incl. Iconify).
 *
 * Build:  m68k-amigaos-gcc -Os -noixemul -Wall -Wno-pointer-sign -s -o AmiFM AmiFM.c
 *         (or just `make`)
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
    "AddIcon", "Extract", "Pack", "Find", "Swap",   "All",    "None"       /* row 1 */
};
#define NBTN 14
static const UBYTE toolRow[NBTN] = { 0,0,0,0,0,0,0, 1,1,1,1,1,1,1 };
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

static void scanPane(struct Pane *p)
{
    BPTR lock; struct FileInfoBlock *fib; int n = 0;
    int extra = 2;        /* always show "." (reload) and ".." (up), Unix-style */
    struct Process *pr = (struct Process *)FindTask(NULL);
    APTR oldwp = pr->pr_WindowPtr;
    pr->pr_WindowPtr = (APTR)-1L;          /* suppress "insert volume" requesters (e.g. empty DF0:) */
    freeEntries(p); p->top = 0; p->sel = -1;

    lock = Lock((STRPTR)p->path, ACCESS_READ);
    if (!lock) { pr->pr_WindowPtr = oldwp; return; }
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) while (ExNext(lock, fib)) n++;
    UnLock(lock);
    if (n + extra <= 0 || !fib) { if (fib) FreeDosObject(DOS_FIB, fib); pr->pr_WindowPtr = oldwp; return; }

    p->entries = AllocVec(sizeof(struct Entry) * (n + extra), MEMF_CLEAR);
    if (!p->entries) { FreeDosObject(DOS_FIB, fib); pr->pr_WindowPtr = oldwp; return; }

    lock = Lock((STRPTR)p->path, ACCESS_READ);
    if (lock && Examine(lock, fib)) {
        int i = 0;
        while (ExNext(lock, fib) && i < n) {
            struct Entry *e = &p->entries[i];
            struct DateTime dt; char db[20]; db[0] = '\0';
            strncpy(e->name, (char *)fib->fib_FileName, sizeof(e->name) - 1);
            e->isdir = (fib->fib_DirEntryType > 0);
            e->size  = fib->fib_Size;
            if (e->isdir) strcpy(e->sizestr, "<DIR>");
            else fmt(e->sizestr, sizeof(e->sizestr), "%ld", (long)e->size);
            dt.dat_Stamp = fib->fib_Date; dt.dat_Format = FORMAT_DOS; dt.dat_Flags = 0;
            dt.dat_StrDay = NULL; dt.dat_StrDate = (STRPTR)db; dt.dat_StrTime = NULL;
            DateToStr(&dt);
            strncpy(e->datestr, db, sizeof(e->datestr) - 1);
            e->dkey = fib->fib_Date.ds_Days * 1440L + fib->fib_Date.ds_Minute;
            i++;
        }
        p->count = i;
    }
    if (lock) UnLock(lock);
    FreeDosObject(DOS_FIB, fib);
    pr->pr_WindowPtr = oldwp;
    {   /* prepend the "." (reload) and ".." (parent) nav entries */
        static const char *nav[2] = { ".", ".." };
        int k;
        for (k = 0; k < extra && k < 2; k++) {
            struct Entry *e = &p->entries[p->count];
            strcpy(e->name, nav[k]);
            e->isdir = TRUE; e->size = 0; e->dkey = 0; e->tagged = FALSE;
            strcpy(e->sizestr, "<DIR>"); e->datestr[0] = '\0';
            p->count++;
        }
    }
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

static BOOL copyFile(const char *src, const char *dst)
{
    static char buf[8192];
    BPTR in, out; LONG n; BOOL ok = TRUE;
    if (!(in = Open((STRPTR)src, MODE_OLDFILE))) return FALSE;
    if (!(out = Open((STRPTR)dst, MODE_NEWFILE))) { Close(in); return FALSE; }
    while ((n = Read(in, buf, sizeof(buf))) > 0)
        if (Write(out, buf, n) != n) { ok = FALSE; break; }
    if (n < 0) ok = FALSE;
    Close(in); Close(out);
    return ok;
}

static void refresh1(struct Pane *p) { scanPane(p); syncScroller(p); drawPane(p); }

/* ---- tagging / multi-select working set --------------------------------- */

struct WItem { char name[108]; BOOL isdir; };
#define MAXWORK 256
static struct WItem g_work[MAXWORK];   /* shared (ops are sequential) — keeps ~28KB off the stack */

static void clearTags(struct Pane *p)
{
    int i; for (i = 0; i < p->count; i++) p->entries[i].tagged = FALSE;
}

#define IS_DOTDOT(nm) ((nm)[0] == '.' && (nm)[1] == '.' && (nm)[2] == '\0')
#define IS_DOT(nm)    ((nm)[0] == '.' && (nm)[1] == '\0')
#define IS_NAV(nm)    (IS_DOT(nm) || IS_DOTDOT(nm))

/* Snapshot the working set BY NAME (tagged rows, or the cursor row if none
 * tagged) so destructive ops can rescan without index drift. ".." is excluded. */
static int gatherWork(struct Pane *p, struct WItem *w, int max)
{
    int n = 0, i;
    for (i = 0; i < p->count && n < max; i++)
        if (p->entries[i].tagged && !IS_NAV(p->entries[i].name)) {
            strcpy(w[n].name, p->entries[i].name); w[n].isdir = p->entries[i].isdir; n++;
        }
    if (n == 0 && p->sel >= 0 && p->sel < p->count && !IS_NAV(p->entries[p->sel].name)) {
        strcpy(w[0].name, p->entries[p->sel].name); w[0].isdir = p->entries[p->sel].isdir; n = 1;
    }
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
    struct Pane *p = active(); struct WItem *w = g_work; char full[384], q[80];
    int n = gatherWork(p, w, MAXWORK), i, fail = 0;
    if (!n) { info("Select or tag items first."); return; }
    if (n == 1) { if (!confirm("Delete this item?", w[0].name)) return; }
    else { fmt(q, sizeof q, "Delete %d tagged items?", n); if (!confirm(q, "")) return; }
    for (i = 0; i < n; i++) {
        strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)w[i].name, sizeof full);
        if (!DeleteFile((STRPTR)full)) fail++;
    }
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
    struct Pane *s = active(), *d = other(); struct WItem *w = g_work;
    char src[384], dst[384], m[80]; int n = gatherWork(s, w, MAXWORK), i;
    int copied = 0, fail = 0, skipdir = 0, skipover = 0;
    BOOL yesall = FALSE, cancelled = FALSE;
    if (!n) { info("Select or tag items first."); return; }
    if (ci_cmp(s->path, d->path) == 0) {       /* same drawer: copyFile would truncate the file onto itself */
        info("Both panes show the same drawer.\nNothing to copy."); return;
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
    clearTags(s); refresh1(d); drawPane(s);
    if (cancelled || fail || skipdir || skipover)
        { fmt(m, sizeof m, "%d copied, %d failed, %d skipped, %d drawer(s).",
                copied, fail, skipover, skipdir); info(m); }
}

static void opMove(void)   /* active pane -> other pane (same-volume rename) */
{
    struct Pane *s = active(), *d = other(); struct WItem *w = g_work;
    char oldp[384], newp[384], m[80]; int n = gatherWork(s, w, MAXWORK), i, fail = 0;
    if (!n) { info("Select or tag items first."); return; }
    if (ci_cmp(s->path, d->path) == 0) { info("Both panes show the same drawer."); return; }
    for (i = 0; i < n; i++) {
        strcpy(oldp, s->path); AddPart((STRPTR)oldp, (STRPTR)w[i].name, sizeof oldp);
        strcpy(newp, d->path); AddPart((STRPTR)newp, (STRPTR)w[i].name, sizeof newp);
        if (!Rename((STRPTR)oldp, (STRPTR)newp)) fail++;
    }
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
    struct Pane *p = active(); char cmd[460], full[384];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file first."); return; }
    if (p->entries[p->sel].isdir) { info("That's a drawer (double-click to enter)."); return; }
    strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)p->entries[p->sel].name, sizeof full);
    fmt(cmd, sizeof cmd, "SYS:Utilities/MultiView \"%s\"", full);
    launchCommand(cmd, TRUE);                        /* async: opens its own window */
}

/* Edit: open the selected file in a GUI text editor. Defaults to the 3.2 bundled
 * SYS:Tools/TextEdit; set ENV:EDITOR (e.g. to CygnusEd) to override. */
static void opEdit(void)
{
    struct Pane *p = active(); char full[384], ed[120], cmd[520];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file to edit."); return; }
    if (p->entries[p->sel].isdir) { info("That's a drawer, not a file."); return; }
    strcpy(full, p->path); AddPart((STRPTR)full, (STRPTR)p->entries[p->sel].name, sizeof full);
    if (GetVar((STRPTR)"EDITOR", (STRPTR)ed, sizeof ed, GVF_GLOBAL_ONLY) <= 0)
        strcpy(ed, "SYS:Tools/TextEdit");
    fmt(cmd, sizeof cmd, "%s \"%s\"", ed, full);
    launchCommand(cmd, TRUE);                         /* async: editor opens its own window */
}

static void opExtract(void)   /* extract tagged archive(s) into the other pane */
{
    struct Pane *s = active(), *d = other(); struct WItem *w = g_work;
    char cmd[700], arc[384], m[80]; int n = gatherWork(s, w, MAXWORK), i, got = 0, skip = 0;
    if (!n) { info("Select or tag an archive first."); return; }
    for (i = 0; i < n; i++) {
        if (w[i].isdir || !isArchive(w[i].name)) { skip++; continue; }
        strcpy(arc, s->path); AddPart((STRPTR)arc, (STRPTR)w[i].name, sizeof arc);
        fmt(cmd, sizeof cmd, "xadUnFile \"%s\" DEST \"%s\" OVERWRITE", arc, d->path);
        launchCommand(cmd, FALSE);                    /* extract into the destination pane */
        got++;
    }
    clearTags(s); refresh1(d); drawPane(s);
    if (skip) { fmt(m, sizeof m, "%d extracted, %d skipped (not an archive).", got, skip); info(m); }
}

static void opPack(void)
{
    struct Pane *p = active(); char name[80] = "", cmd[420];
    if (p->sel < 0 || p->sel >= p->count) { info("Select a file/drawer to pack."); return; }
    if (IS_NAV(p->entries[p->sel].name)) return;
    if (!getString("Archive name (.lha added):", name, sizeof name)) return;
    fmt(cmd, sizeof cmd, "lha a \"%s.lha\" \"%s\"", name, p->entries[p->sel].name);
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
    int chosen = -1, i, hov = -1, top = 0, maxl = 12;
    WORD tH = scr->WBorTop + (scr->Font ? scr->Font->ta_YSize : 8) + 1;
    WORD w, h;
    BOOL done = FALSE;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) { int l = strlen(base + (long)i * stride); if (l > maxl) maxl = l; }
    if (maxl > 52) maxl = 52;
    w = maxl * g_cw + PICK_SBW + 14; if (w < 150) w = 150; if (w > scr->Width - 20) w = scr->Width - 20;
    h = tH + (n < 18 ? n : 18) * g_rowH + 4 + scr->WBorBottom; if (h > scr->Height - 10) h = scr->Height - 10;
    vw = OpenWindowTags(NULL, WA_Left, (scr->Width - w) / 2, WA_Top, (scr->Height - h) / 2,
        WA_Width, w, WA_Height, h, WA_MinWidth, 120, WA_MinHeight, 70,
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
    static char vols[80][40];          /* static: keep this off the small stack */
    int nv = getVolumes(vols, 80), idx;
    if (nv <= 0) return FALSE;
    idx = chooseList("Pick volume / device / assign", (char *)vols, 40, nv);
    if (idx < 0) return FALSE;
    strncpy(out, vols[idx], outlen - 1); out[outlen - 1] = '\0';
    return TRUE;
}

/* ---- Find: recursive name-pattern search ------------------------------- */

static void findWalk(const char *dir, const UBYTE *pat, char res[][256], int *nr, int maxr, int depth)
{
    BPTR lock; struct FileInfoBlock *fib;
    if (*nr >= maxr || depth > 16) return;
    lock = Lock((STRPTR)dir, ACCESS_READ);
    if (!lock) return;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib) && *nr < maxr) {
            char child[256];
            BOOL isdir = (fib->fib_DirEntryType > 0);
            strcpy(child, dir);
            AddPart((STRPTR)child, (STRPTR)fib->fib_FileName, sizeof child);
            if (MatchPatternNoCase((UBYTE *)pat, (STRPTR)fib->fib_FileName)) {
                strncpy(res[*nr], child, 255); res[*nr][255] = '\0'; (*nr)++;
            }
            if (isdir) findWalk(child, pat, res, nr, maxr, depth + 1);
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
}

static void opFind(void)
{
    static char res[200][256];
    struct Pane *p = active();
    char raw[80] = "", patbuf[200]; UBYTE pat[260];
    int nr = 0, idx, i;
    struct Process *pr; APTR oldwp;
    if (!getString("Find (name, or #? pattern):", raw, sizeof raw)) return;
    if (raw[0] == '\0') return;
    if (!strpbrk(raw, "#?*%([|]~"))                          /* plain text -> substring match */
        fmt(patbuf, sizeof patbuf, "#?%s#?", raw);
    else { strncpy(patbuf, raw, sizeof patbuf - 1); patbuf[sizeof patbuf - 1] = '\0'; }
    if (ParsePatternNoCase((STRPTR)patbuf, (STRPTR)pat, sizeof pat) < 0) { info("Bad search pattern."); return; }
    pr = (struct Process *)FindTask(NULL); oldwp = pr->pr_WindowPtr; pr->pr_WindowPtr = (APTR)-1L;
    findWalk(p->path, pat, res, &nr, 200, 0);
    pr->pr_WindowPtr = oldwp;
    if (nr == 0) { info("No matches found."); return; }
    idx = chooseList("Find results - click to open", (char *)res, 256, nr);
    if (idx < 0) return;
    {   /* navigate: split the chosen full path into directory + filename */
        char full[256], dir[256], fn[120]; int k, sep = -1;
        strcpy(full, res[idx]); k = strlen(full);
        for (i = k - 1; i >= 0; i--) if (full[i] == '/' || full[i] == ':') { sep = i; break; }
        if (sep < 0) { strcpy(dir, p->path); strcpy(fn, full); }
        else if (full[sep] == ':') { memcpy(dir, full, sep + 1); dir[sep + 1] = '\0'; strcpy(fn, full + sep + 1); }
        else { memcpy(dir, full, sep); dir[sep] = '\0'; strcpy(fn, full + sep + 1); }
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
}

/* ---- right-button menu, window open, iconify --------------------------- */

static struct NewMenu g_newmenu[] = {
    { NM_TITLE, (STRPTR)"AmiFM",       0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Reload",      (STRPTR)"R", 0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Iconify",     (STRPTR)"I", 0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,   0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Quit",        (STRPTR)"Q", 0, 0, (APTR)0 },
    { NM_TITLE, (STRPTR)"Selection",   0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"View",        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Edit",        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Extract",     0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Pack",        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,   0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Rename",      0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Delete",      0,           0, 0, (APTR)0 },
    { NM_TITLE, (STRPTR)"Pane",        0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Parent",      0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Make Drawer", 0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Find...",     0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Swap Panes",  0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,   0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Select All",  0,           0, 0, (APTR)0 },
    { NM_ITEM,  (STRPTR)"Clear Tags",  0,           0, 0, (APTR)0 },
    { NM_END,   0,                     0,           0, 0, (APTR)0 }
};

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
    win = OpenWindowTags(NULL,
        WA_Left, 30, WA_Top, (ULONG)(scr->BarHeight + 6),
        WA_Width,  (ULONG)(scr->Width  - 90),
        WA_Height, (ULONG)(scr->Height - scr->BarHeight - 56),
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

    strcpy(panes[0].path, "SYS:");
    strcpy(panes[1].path, "RAM:");
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
                while (mnum != MENUNULL) {
                    struct MenuItem *mi = ItemAddress(g_menu, mnum);
                    UWORD mn = MENUNUM(mnum), in = ITEMNUM(mnum);
                    if (mn == 0) {                       /* AmiFM */
                        if (in == 0) opReload(); else if (in == 1) iconify(); else if (in == 3) done = TRUE;
                    } else if (mn == 1) {                /* Selection */
                        switch (in) { case 0: opView(); break; case 1: opEdit(); break;
                            case 2: opExtract(); break; case 3: opPack(); break;
                            case 5: opRename(); break; case 6: opDelete(); break; }
                    } else if (mn == 2) {                /* Pane */
                        switch (in) { case 0: opParent(); break; case 1: opMakeDir(); break;
                            case 2: opFind(); break; case 3: opSwap(); break;
                            case 5: opSelectAll(); break; case 6: opClearTags(); break; }
                    }
                    mnum = mi ? mi->NextSelect : MENUNULL;
                }
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
                                case 10: opFind();   break; case 11: opSwap();   break;
                                case 12: opSelectAll(); break; case 13: opClearTags(); break; }
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
