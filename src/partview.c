/*
 * partview.c — Partition view window for DiskPart.
 *
 * Layout mirrors AmigaPart:
 *   ┌─ Disk Information ──────────────────────────────────┐
 *   │ Device / Size / Geometry / Model / RDB status        │
 *   ├─ Disk Map ──────────────────────────────────────────┤
 *   │  [RDB] [DH0────────] [DH1────────] [free  ·····]    │
 *   │  Cyl 0            Free: 250 MB           Cyl 1039   │
 *   ├─ Partitions ────────────────────────────────────────┤
 *   │  Drive   Lo Cyl  Hi Cyl  Filesystem    Size   Boot  │
 *   │  DH0          1     519  FFS         250 MB       0 │
 *   ├─ Buttons ───────────────────────────────────────────┤
 *   │  [Init RDB] [Add] [Edit] [Delete]          [Back]   │
 *   └─────────────────────────────────────────────────────┘
 *
 * Drag resize: click and drag partition edges in the map to resize.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <dos/dos.h>
#include <libraries/asl.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include "clib.h"
#include "rdb.h"
#include "partview.h"

/* ------------------------------------------------------------------ */
/* External library bases (defined in main.c)                          */
/* ------------------------------------------------------------------ */

extern struct ExecBase      *SysBase;
extern struct DosLibrary    *DOSBase;
extern struct Library       *AslBase;
extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase       *GfxBase;
extern struct Library       *GadToolsBase;

/* ------------------------------------------------------------------ */
/* Mouse button codes (from devices/inputevent.h IECODE_* values)     */
/* ------------------------------------------------------------------ */

#ifndef SELECTDOWN
#define SELECTDOWN 0x68
#define SELECTUP   0xE8
#endif

/* ------------------------------------------------------------------ */
/* Gadget IDs                                                           */
/* ------------------------------------------------------------------ */

#define GID_PARTLIST  1
#define GID_INITRDB   2
#define GID_ADD       3
#define GID_EDIT      4
#define GID_DELETE    5
#define GID_FILESYS   6
#define GID_WRITE     7
#define GID_BACK      8

/* ------------------------------------------------------------------ */
/* Partition colours — match AmigaPart COLORS list                     */
/* ------------------------------------------------------------------ */

#define NUM_PART_COLORS 8
static const UBYTE PART_R[NUM_PART_COLORS]={0x4A,0xE6,0x27,0x8E,0xE7,0x16,0xF3,0x29};
static const UBYTE PART_G[NUM_PART_COLORS]={0x90,0x7E,0xAE,0x44,0x4C,0xA0,0x9C,0x80};
static const UBYTE PART_B[NUM_PART_COLORS]={0xD9,0x22,0x60,0xAD,0x3C,0x85,0x12,0xB9};
#define C32(b) (((ULONG)(b)<<24)|((ULONG)(b)<<16)|((ULONG)(b)<<8)|(ULONG)(b))

/* ------------------------------------------------------------------ */
/* Partition list strings (columnar, monospace-friendly)               */
/* ------------------------------------------------------------------ */

/* Column header drawn just above the listview gadget */
static const char PART_HDR[] =
    "  Drive   Lo Cyl Hi Cyl  FileSystem       Size  Boot";

static char        part_strs[MAX_PARTITIONS][80];
static struct Node part_nodes[MAX_PARTITIONS];
static struct List part_list;

static void list_init(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void build_part_list(struct RDBInfo *rdb, WORD sel)
{
    UWORD i;
    list_init(&part_list);
    if (!rdb || !rdb->valid || rdb->num_parts == 0) return;

    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi = &rdb->parts[i];
        char dt[16], sz[16];
        ULONG cyls  = pi->high_cyl - pi->low_cyl + 1;
        ULONG heads = pi->heads   > 0 ? pi->heads   : rdb->heads;
        ULONG secs  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
        ULONG bsz   = pi->block_size > 0 ? pi->block_size : 512;
        UQUAD bytes = (UQUAD)cyls * heads * secs * bsz;

        FormatDosType(pi->dos_type, dt);
        FormatSize(bytes, sz);

        /* ">" marker for selected row, space otherwise */
        sprintf(part_strs[i], "%c %-7s %6lu  %6lu  %-12s  %7s  %4ld",
                ((WORD)i == sel) ? '>' : ' ',
                pi->drive_name,
                (unsigned long)pi->low_cyl,
                (unsigned long)pi->high_cyl,
                dt, sz,
                (long)pi->boot_pri);

        part_nodes[i].ln_Name = part_strs[i];
        part_nodes[i].ln_Type = NT_USER;
        part_nodes[i].ln_Pri  = 0;
        AddTail(&part_list, &part_nodes[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Pen allocation                                                       */
/* ------------------------------------------------------------------ */

static LONG part_pens[NUM_PART_COLORS];
static LONG bg_pen;      /* dark navy background for the map  */
static LONG rdb_pen;     /* muted gray for RDB reserved area  */

static void alloc_pens(struct Screen *scr)
{
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    struct TagItem   nt[] = { { TAG_DONE, 0 } };
    UWORD i;
    for (i = 0; i < NUM_PART_COLORS; i++)
        part_pens[i] = ObtainBestPenA(cm,
            C32(PART_R[i]), C32(PART_G[i]), C32(PART_B[i]), nt);
    bg_pen  = ObtainBestPenA(cm, C32(0x2a), C32(0x2a), C32(0x3a), nt);
    rdb_pen = ObtainBestPenA(cm, C32(0x55), C32(0x55), C32(0x66), nt);
}

static void free_pens(struct Screen *scr)
{
    struct ColorMap *cm = scr->ViewPort.ColorMap;
    UWORD i;
    for (i = 0; i < NUM_PART_COLORS; i++)
        if (part_pens[i] >= 0) { ReleasePen(cm,(ULONG)part_pens[i]); part_pens[i]=-1; }
    if (bg_pen  >= 0) { ReleasePen(cm,(ULONG)bg_pen);  bg_pen  = -1; }
    if (rdb_pen >= 0) { ReleasePen(cm,(ULONG)rdb_pen); rdb_pen = -1; }
}

/* ------------------------------------------------------------------ */
/* Disk map drawing (matches AmigaPart _draw_map style)                */
/* ------------------------------------------------------------------ */

static void draw_map(struct Window *win, struct RDBInfo *rdb, WORD sel,
                     WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp  = win->RPort;
    WORD  fh = rp->TxHeight;
    WORD  fb = rp->TxBaseline;
    LONG  fill  = (bg_pen  >= 0) ? bg_pen  : 0;
    LONG  rfill = (rdb_pen >= 0) ? rdb_pen : 2;
    WORD  i;

    /* Map inner area — leave 1px border all round */
    WORD  mx  = bx + 1;
    WORD  my  = by + 1;
    UWORD mw  = bw - 2;
    UWORD mh  = bh - 2;

    /* Outer border */
    SetAPen(rp, 2);
    SetDrMd(rp, JAM1);
    Move(rp, bx,          by);     Draw(rp, bx+(WORD)bw, by);
    Draw(rp, bx+(WORD)bw, by+(WORD)bh);
    Draw(rp, bx,          by+(WORD)bh);
    Draw(rp, bx,          by);

    /* Background — free space */
    SetAPen(rp, fill);
    SetDrMd(rp, JAM2);
    RectFill(rp, mx, my, mx+(WORD)mw-1, my+(WORD)mh-1);

    if (!rdb || !rdb->valid) {
        const char *msg = "No RDB — use Init RDB to create partitions";
        UWORD mlen = strlen(msg);
        WORD  tw   = rp->TxWidth ? (WORD)(mlen*(UWORD)rp->TxWidth):(WORD)(mlen*8);
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, bx + ((WORD)bw-(WORD)tw)/2, by+((WORD)bh-fh)/2+fb);
        Text(rp, msg, mlen);
        return;
    }

    {
        ULONG lo    = rdb->lo_cyl;
        ULONG hi    = rdb->hi_cyl;
        ULONG total = hi + 1;   /* full disk cylinder count (including RDB area) */

#define MAP_X(cyl) ((WORD)(mx + (WORD)((UQUAD)(cyl) * mw / total)))

        /* RDB reserved area (cylinder 0 .. lo_cyl-1) */
        if (lo > 0) {
            WORD rx2 = MAP_X(lo);
            if (rx2 > mx + 1) {
                SetAPen(rp, rfill);
                SetDrMd(rp, JAM2);
                RectFill(rp, mx, my, rx2-1, my+(WORD)mh-1);
                if (rx2 - mx > 24) {
                    SetAPen(rp, 1);
                    SetDrMd(rp, JAM1);
                    Move(rp, mx + (rx2-mx)/2 - 6, by+((WORD)bh-fh)/2+fb);
                    Text(rp, "RDB", 3);
                }
            }
        }

        /* Partition blocks */
        for (i = 0; i < (WORD)rdb->num_parts; i++) {
            struct PartInfo *pi  = &rdb->parts[i];
            WORD  px1 = MAP_X(pi->low_cyl);
            WORD  px2 = MAP_X(pi->high_cyl + 1);
            LONG  pen;
            WORD  pw;

            if (px2 < px1 + 2) px2 = px1 + 2;
            pen = part_pens[i % NUM_PART_COLORS];
            if (pen < 0) pen = (i % 3) + 3;

            SetAPen(rp, pen);
            SetDrMd(rp, JAM2);
            RectFill(rp, px1, my, px2-1, my+(WORD)mh-1);

            /* Border */
            SetAPen(rp, 2);
            SetDrMd(rp, JAM1);
            Move(rp, px1, my);             Draw(rp, px2-1, my);
            Move(rp, px1, my+(WORD)mh-1);  Draw(rp, px2-1, my+(WORD)mh-1);
            Move(rp, px1, my);             Draw(rp, px1,   my+(WORD)mh-1);
            Move(rp, px2-1, my);           Draw(rp, px2-1, my+(WORD)mh-1);

            /* Drive name + size label */
            pw = px2 - px1;
            if (pw > 12) {
                char  sz[16];
                UWORD slen;
                ULONG cyls2  = pi->high_cyl - pi->low_cyl + 1;
                ULONG heads2 = pi->heads   > 0 ? pi->heads   : rdb->heads;
                ULONG secs2  = pi->sectors > 0 ? pi->sectors : rdb->sectors;
                UQUAD bytes2 = (UQUAD)cyls2 * heads2 * secs2 * 512UL;
                WORD  txw    = rp->TxWidth ? (WORD)rp->TxWidth : 8;
                WORD  max_c  = (pw - 4) / txw;
                char  *nm    = pi->drive_name;
                UWORD nlen   = strlen(nm);
                WORD  block_top; /* top of two-line text block */

                FormatSize(bytes2, sz);
                slen = strlen(sz);

                if ((WORD)nlen > max_c) nlen = (UWORD)max_c;
                if ((WORD)slen > max_c) slen = (UWORD)max_c;

                /* Centre the two-line block vertically inside the map bar */
                block_top = my + ((WORD)mh - (fh * 2 + 1)) / 2;

                SetAPen(rp, 1);
                SetDrMd(rp, JAM1);

                if (nlen > 0) {
                    WORD tw = (WORD)(nlen * (UWORD)txw);
                    Move(rp, px1 + (pw - tw) / 2, block_top + fb);
                    Text(rp, nm, nlen);
                }
                if (slen > 0 && (WORD)mh >= fh * 2 + 4) {
                    WORD tw = (WORD)(slen * (UWORD)txw);
                    Move(rp, px1 + (pw - tw) / 2, block_top + fh + 1 + fb);
                    Text(rp, sz, slen);
                }
            }
        }

        /* Selection highlight: 3-px bright frame + dark shadow frame */
        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
            struct PartInfo *sp  = &rdb->parts[sel];
            WORD sx1 = MAP_X(sp->low_cyl);
            WORD sx2 = MAP_X(sp->high_cyl + 1);
            WORD bsz = 3;   /* frame thickness in pixels */
            if (sx2 < sx1 + 2) sx2 = sx1 + 2;

            /* Dark shadow frame 1px outside the bright frame for contrast */
            SetAPen(rp, 2);
            SetDrMd(rp, JAM2);
            if (sx1 > mx) {
                RectFill(rp, sx1-1, my,           sx1-1, my+(WORD)mh-1);
            }
            if (sx2 < mx+(WORD)mw) {
                RectFill(rp, sx2,   my,           sx2,   my+(WORD)mh-1);
            }
            RectFill(rp, sx1, my-1 > my ? my-1 : my, sx2-1, my-1 > my ? my-1 : my);

            /* Bright (pen 1) 3-px thick inner frame via four strips */
            SetAPen(rp, 1);
            /* Top */
            RectFill(rp, sx1,        my,              sx2-1,        my+bsz-1);
            /* Bottom */
            RectFill(rp, sx1,        my+(WORD)mh-bsz, sx2-1,        my+(WORD)mh-1);
            /* Left */
            RectFill(rp, sx1,        my+bsz,          sx1+bsz-1,    my+(WORD)mh-bsz-1);
            /* Right */
            RectFill(rp, sx2-bsz,    my+bsz,          sx2-1,        my+(WORD)mh-bsz-1);
        }

#undef MAP_X

        /* Axis labels — lo/hi cylinder only; free space is shown in info area */
        {
            char lo_str[24], hi_str[24];
            WORD label_y = by + (WORD)bh + 2 + fb;

            sprintf(lo_str, "Cyl %lu", (unsigned long)lo);
            sprintf(hi_str, "Cyl %lu", (unsigned long)hi);

            SetAPen(rp, 1);
            SetDrMd(rp, JAM1);
            Move(rp, bx, label_y);
            Text(rp, lo_str, strlen(lo_str));

            {
                UWORD hlen = strlen(hi_str);
                WORD  txw  = rp->TxWidth ? (WORD)rp->TxWidth : 8;
                WORD  htw  = (WORD)(hlen * (UWORD)txw);
                Move(rp, bx+(WORD)bw-htw, label_y);
                Text(rp, hi_str, hlen);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drag resize info — replaces axis labels during an active drag       */
/* Shows "DH0: Cyl 1 - 519  (250 MB)" centred below the map bar.     */
/* ------------------------------------------------------------------ */

static void draw_drag_info(struct Window *win, const struct RDBInfo *rdb,
                            WORD drag_part,
                            WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp = win->RPort;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    WORD  txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    WORD  label_y = by + (WORD)bh + 2 + fb;
    char  info[64];
    UWORD ilen;
    const struct PartInfo *pi = &rdb->parts[drag_part];
    ULONG cyls  = pi->high_cyl >= pi->low_cyl
                  ? pi->high_cyl - pi->low_cyl + 1 : 0;
    UQUAD bytes = (UQUAD)cyls * rdb->heads * rdb->sectors * 512UL;
    char  sz[16];

    FormatSize(bytes, sz);
    sprintf(info, "%s: Cyl %lu - %lu  (%s)",
            pi->drive_name,
            (unsigned long)pi->low_cyl,
            (unsigned long)pi->high_cyl,
            sz);

    /* Erase axis label strip */
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, bx, by+(WORD)bh+1,
             bx+(WORD)bw, by+(WORD)bh+fh+4);

    /* Draw centred info string */
    ilen = strlen(info);
    {
        WORD  iw2 = (WORD)(ilen * (UWORD)txw);
        WORD  cx  = bx + ((WORD)bw - iw2) / 2;
        if (cx < bx) cx = bx;
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, cx, label_y);
        Text(rp, info, ilen);
    }
}

/* ------------------------------------------------------------------ */
/* New-partition drag overlay — drawn on top of the map during drag    */
/* ------------------------------------------------------------------ */

static void draw_new_part_overlay(struct Window *win,
                                   ULONG lo, ULONG hi,
                                   const struct RDBInfo *rdb,
                                   WORD bx, WORD by, UWORD bw, UWORD bh)
{
    struct RastPort *rp  = win->RPort;
    WORD  mx    = bx + 1;
    UWORD mw    = bw - 2;
    WORD  my    = by + 1;
    UWORD mh    = bh - 2;
    ULONG total = rdb->hi_cyl + 1;
    WORD  px1, px2, pw;
    char  sz[16], info[64];
    UWORD ilen;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    WORD  txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    WORD  label_y = by + (WORD)bh + 2 + fb;
    ULONG cyls, bpc;
    UQUAD bytes;
    LONG  pen;

    if (total == 0) return;
    px1 = (WORD)(mx + (WORD)((UQUAD)lo       * mw / total));
    px2 = (WORD)(mx + (WORD)((UQUAD)(hi + 1) * mw / total));
    if (px2 < px1 + 2) px2 = px1 + 2;
    pw = px2 - px1;

    cyls  = (hi >= lo) ? (hi - lo + 1) : 1;
    bpc   = rdb->heads * rdb->sectors * 512UL;
    bytes = (UQUAD)cyls * bpc;
    FormatSize(bytes, sz);

    /* Fill with the color this partition would get when added */
    pen = part_pens[rdb->num_parts % NUM_PART_COLORS];
    if (pen < 0) pen = (LONG)(rdb->num_parts % 3) + 3;
    SetAPen(rp, pen);
    SetDrMd(rp, JAM2);
    RectFill(rp, px1, my, px2-1, my+(WORD)mh-1);

    /* Bright double border */
    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, px1,   my);             Draw(rp, px2-1, my);
    Move(rp, px1,   my+(WORD)mh-1);  Draw(rp, px2-1, my+(WORD)mh-1);
    Move(rp, px1,   my);             Draw(rp, px1,   my+(WORD)mh-1);
    Move(rp, px2-1, my);             Draw(rp, px2-1, my+(WORD)mh-1);
    if (pw > 4 && (WORD)mh > 4) {
        Move(rp, px1+1,   my+1);           Draw(rp, px2-2, my+1);
        Move(rp, px1+1,   my+(WORD)mh-2);  Draw(rp, px2-2, my+(WORD)mh-2);
        Move(rp, px1+1,   my+1);           Draw(rp, px1+1, my+(WORD)mh-2);
        Move(rp, px2-2,   my+1);           Draw(rp, px2-2, my+(WORD)mh-2);
    }

    /* Size hint centred inside the box */
    {
        UWORD slen = strlen(sz);
        WORD  tw   = (WORD)(slen * (UWORD)txw);
        if (pw > tw + 4) {
            Move(rp, px1 + (pw - tw) / 2, my + ((WORD)mh - fh) / 2 + fb);
            Text(rp, sz, slen);
        }
    }

    /* Info strip below map */
    sprintf(info, "New: Cyl %lu - %lu  (%s)",
            (unsigned long)lo, (unsigned long)hi, sz);
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, bx, by+(WORD)bh+1, bx+(WORD)bw, by+(WORD)bh+fh+4);
    ilen = strlen(info);
    {
        WORD iw2 = (WORD)(ilen * (UWORD)txw);
        WORD cx  = bx + ((WORD)bw - iw2) / 2;
        if (cx < bx) cx = bx;
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, cx, label_y);
        Text(rp, info, ilen);
    }
}

/* ------------------------------------------------------------------ */
/* Disk information section — drawn as text rows above the map         */
/* ------------------------------------------------------------------ */

static void draw_info(struct Window *win, const char *devname, ULONG unit,
                      struct RDBInfo *rdb,
                      WORD ix, WORD iy, UWORD iw)
{
    struct RastPort *rp = win->RPort;
    char   line1[120], line2[120];
    char   sz[16];
    WORD   fb  = rp->TxBaseline;
    WORD   fh  = rp->TxHeight;
    WORD   txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;

    /* Erase the info area */
    SetAPen(rp, 0);
    SetDrMd(rp, JAM2);
    RectFill(rp, ix, iy, ix+(WORD)iw-1, iy+fh*2+4);

    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);

    if (rdb && rdb->cylinders > 0) {
        UQUAD total = (UQUAD)rdb->cylinders * rdb->heads * rdb->sectors * 512;
        FormatSize(total, sz);
        sprintf(line1, "Device: %s/%lu    Size: %s    Geometry: %lu x %lu x %lu",
                devname, (unsigned long)unit, sz,
                (unsigned long)rdb->cylinders,
                (unsigned long)rdb->heads,
                (unsigned long)rdb->sectors);
    } else {
        sprintf(line1, "Device: %s/%lu    Size: unknown    Geometry: unknown",
                devname, (unsigned long)unit);
    }

    if (rdb && rdb->valid) {
        char model[32], fsz[16];
        ULONG free_cyls = rdb->hi_cyl - rdb->lo_cyl + 1;
        UQUAD free_bytes;
        UWORD fi;

        for (fi = 0; fi < rdb->num_parts; fi++) {
            ULONG used = rdb->parts[fi].high_cyl - rdb->parts[fi].low_cyl + 1;
            if (free_cyls >= used) free_cyls -= used;
        }
        free_bytes = (UQUAD)free_cyls * rdb->heads * rdb->sectors * 512UL;
        FormatSize(free_bytes, fsz);

        model[0] = '\0';
        if (rdb->disk_vendor[0] || rdb->disk_product[0])
            sprintf(model, "%s %s", rdb->disk_vendor, rdb->disk_product);

        if (model[0])
            sprintf(line2, "Model: %-20s  RDB: %u partition%s  Free: %s",
                    model, (unsigned)rdb->num_parts,
                    rdb->num_parts == 1 ? "" : "s", fsz);
        else
            sprintf(line2, "RDB: %u partition%s  Free: %s",
                    (unsigned)rdb->num_parts,
                    rdb->num_parts == 1 ? "" : "s", fsz);
    } else {
        sprintf(line2, "RDB: Not found  (disk is unpartitioned)");
    }

    /* Clip line1 to window width */
    {
        UWORD l1 = strlen(line1);
        UWORD max_c = (UWORD)((iw - 4) / (UWORD)txw);
        if (l1 > max_c) l1 = max_c;
        Move(rp, ix + 2, iy + fb);
        Text(rp, line1, l1);
    }
    {
        UWORD l2 = strlen(line2);
        UWORD max_c = (UWORD)((iw - 4) / (UWORD)txw);
        if (l2 > max_c) l2 = max_c;
        Move(rp, ix + 2, iy + fh + 2 + fb);
        Text(rp, line2, l2);
    }
}

/* ------------------------------------------------------------------ */
/* Column header — drawn just above the listview gadget                */
/* ------------------------------------------------------------------ */

static void draw_col_header(struct Window *win, WORD hx, WORD hy, UWORD hw)
{
    struct RastPort *rp  = win->RPort;
    WORD  fb  = rp->TxBaseline;
    WORD  fh  = rp->TxHeight;
    UWORD len = strlen(PART_HDR);
    WORD  txw = rp->TxWidth ? (WORD)rp->TxWidth : 8;
    UWORD max_c = (UWORD)((hw - 4) / (UWORD)txw);
    if (len > max_c) len = max_c;

    /* Background strip */
    SetAPen(rp, 2);
    SetDrMd(rp, JAM2);
    RectFill(rp, hx, hy, hx+(WORD)hw-1, hy+fh+1);

    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, hx + 4, hy + fb);
    Text(rp, PART_HDR, len);
}

/* ------------------------------------------------------------------ */
/* Draw all static text elements (called on open and on refresh)       */
/* ------------------------------------------------------------------ */

static void draw_static(struct Window *win, const char *devname, ULONG unit,
                         struct RDBInfo *rdb,
                         WORD ix, WORD iy, UWORD iw,   /* info section */
                         WORD bx, WORD by, UWORD bw, UWORD bh, /* map */
                         WORD hx, WORD hy, UWORD hw,   /* col header */
                         WORD sel)
{
    draw_info(win, devname, unit, rdb, ix, iy, iw);
    draw_map (win, rdb, sel, bx, by, bw, bh);
    draw_col_header(win, hx, hy, hw);
}

/* ------------------------------------------------------------------ */
/* Listview refresh                                                    */
/* ------------------------------------------------------------------ */

static void refresh_listview(struct Window *win, struct Gadget *lv_gad,
                              struct RDBInfo *rdb, WORD sel)
{
    struct TagItem detach[]   = { { GTLV_Labels, TAG_IGNORE        }, { TAG_DONE, 0 } };
    struct TagItem reattach[] = { { GTLV_Labels, (ULONG)&part_list }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(lv_gad, win, NULL, detach);
    build_part_list(rdb, sel);
    GT_SetGadgetAttrsA(lv_gad, win, NULL, reattach);
}

/* Sync the ">" marker without a full content rebuild */
static void sync_listview_sel(struct Window *win, struct Gadget *lv_gad,
                               struct RDBInfo *rdb, WORD sel)
{
    refresh_listview(win, lv_gad, rdb, sel);
}

/* ------------------------------------------------------------------ */
/* Free cylinder range                                                 */
/* ------------------------------------------------------------------ */

static void find_free_range(const struct RDBInfo *rdb, ULONG *lo, ULONG *hi)
{
    /* Sort partition ranges by low_cyl (insertion sort — n is small),
       then scan for the first gap including holes left by deleted partitions. */
    ULONG  starts[MAX_PARTITIONS];
    ULONG  ends[MAX_PARTITIONS];
    UWORD  n = rdb->num_parts;
    UWORD  i, j;
    ULONG  cursor;

    for (i = 0; i < n; i++) {
        starts[i] = rdb->parts[i].low_cyl;
        ends[i]   = rdb->parts[i].high_cyl;
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (starts[j] < starts[i]) {
                ULONG t;
                t = starts[i]; starts[i] = starts[j]; starts[j] = t;
                t = ends[i];   ends[i]   = ends[j];   ends[j]   = t;
            }
        }
    }

    cursor = rdb->lo_cyl;
    for (i = 0; i < n; i++) {
        if (starts[i] > cursor) {
            *lo = cursor;
            *hi = starts[i] - 1;
            return;
        }
        if (ends[i] + 1 > cursor)
            cursor = ends[i] + 1;
    }
    *lo = cursor;
    *hi = rdb->hi_cyl;
}

/* ------------------------------------------------------------------ */
/* Built-in (ROM) filesystem types — always available                  */
/* ------------------------------------------------------------------ */

static const struct { const char *name; ULONG dostype; } builtin_fs[] = {
    { "OFS",      0x444F5300UL },
    { "FFS",      0x444F5301UL },
    { "FFS+Intl", 0x444F5303UL },
};
#define NUM_BUILTIN_FS 3

/* BufMemType — maps cycle index ↔ MEMF_* value */
static const char * const bufmem_labels[] = {
    "Any", "Chip", "Fast", "24-bit DMA", NULL
};
static const ULONG bufmem_values[] = { 0UL, 2UL, 4UL, 8UL };
#define NUM_BUFMEM_TYPES 4

static UWORD bufmem_index(ULONG val)
{
    UWORD i;
    for (i = 0; i < NUM_BUFMEM_TYPES; i++)
        if (bufmem_values[i] == val) return i;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Numeric parsing helpers                                             */
/* ------------------------------------------------------------------ */

/* Parse decimal or 0x/0X-prefixed hex string to ULONG */
static ULONG parse_num(const char *s)
{
    ULONG val = 0;
    int   hex = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; hex = 1; }
    while (*s) {
        char c = *s++;
        if (hex) {
            if      (c >= '0' && c <= '9') val = val * 16 + (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f') val = val * 16 + (ULONG)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val = val * 16 + (ULONG)(c - 'A' + 10);
            else break;
        } else {
            if (c >= '0' && c <= '9') val = val * 10 + (ULONG)(c - '0');
            else break;
        }
    }
    return val;
}

/* Parse signed decimal (handles leading '-') */
static LONG parse_long(const char *s)
{
    while (*s == ' ') s++;
    if (*s == '-') return -(LONG)parse_num(s + 1);
    return (LONG)parse_num(s);
}

/*
 * Parse a DosType from either hex ("0x50465303") or a 4-char string ("PFS\3").
 * String rules: up to 4 chars packed big-endian into a ULONG.
 *   \N  (backslash + single decimal digit 0-9) → byte value N (e.g. \3 → 0x03).
 *   Any other char → its ASCII value.
 * Examples: "PFS\3" → 0x50465303, "DOS\0" → 0x444F5300, "DOS\1" → 0x444F5301.
 */
static ULONG parse_dostype(const char *s)
{
    ULONG val = 0;
    UBYTE bytes[4];
    UBYTE i, nb;

    while (*s == ' ') s++;

    /* Hex if starts with 0x/0X or is all hex digits (8 chars) */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_num(s);

    /* String encoding: parse up to 4 characters */
    nb = 0;
    while (*s && nb < 4) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '9') {
            bytes[nb++] = (UBYTE)(s[1] - '0');
            s += 2;
        } else {
            bytes[nb++] = (UBYTE)*s++;
        }
    }
    /* Pad remaining bytes with 0 */
    for (i = nb; i < 4; i++) bytes[i] = 0;

    val = ((ULONG)bytes[0] << 24) | ((ULONG)bytes[1] << 16) |
          ((ULONG)bytes[2] <<  8) |  (ULONG)bytes[3];
    return val;
}

/* ------------------------------------------------------------------ */
/* Partition add / edit dialog                                          */
/*                                                                     */
/* Fields (single-column layout):                                      */
/*   Name, Lo Cylinder, Hi Cylinder, Filesystem, Boot Priority,        */
/*   Bootable (checkbox), Buffers, MaxTransfer, Mask                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Advanced dialog gadget IDs                                          */
/* ------------------------------------------------------------------ */
#define ADLG_BUFFERS     1
#define ADLG_BUFMEMTYPE  2
#define ADLG_BOOTBLOCKS  3
#define ADLG_MAXTRANSFER 4
#define ADLG_MASK        5
#define ADLG_OK          6
#define ADLG_CANCEL      7
#define ADLG_ROWS        5

/* ------------------------------------------------------------------ */
/* Main dialog gadget IDs                                              */
/* ------------------------------------------------------------------ */
#define PDLG_NAME        1
#define PDLG_SIZEMB      3
#define PDLG_TYPE        4
#define PDLG_BOOTPRI     5
#define PDLG_BOOTABLE    6
#define PDLG_DIRSCSI     7
#define PDLG_OK          8
#define PDLG_ADVANCED    9
#define PDLG_CANCEL      10

/* Rows: Name, LoCyl, HiCyl, FS, BootPri, Bootable+DirSCSI */
#define PDLG_ROWS 6

static void partition_advanced_dialog(struct PartInfo *pi)
{
    struct Screen  *scr          = NULL;
    APTR            vi           = NULL;
    struct Gadget  *glist        = NULL;
    struct Gadget  *gctx         = NULL;
    struct Gadget  *buffers_gad  = NULL;
    struct Gadget  *bootblks_gad = NULL;
    struct Gadget  *maxtrans_gad = NULL;
    struct Gadget  *mask_gad     = NULL;
    struct Window  *win          = NULL;
    UWORD           cur_bufmem   = bufmem_index(pi->buf_mem_type);

    char buffers_str[16], bootblks_str[16], maxtrans_str[16], mask_str[16];
    sprintf(buffers_str,  "%lu",     (unsigned long)(pi->num_buffer  > 0 ? pi->num_buffer  : 30));
    sprintf(bootblks_str, "%lu",     (unsigned long)(pi->boot_blocks > 0 ? pi->boot_blocks :  2));
    sprintf(maxtrans_str, "0x%08lX", (unsigned long)pi->max_transfer);
    sprintf(mask_str,     "0x%08lX", (unsigned long)pi->mask);

    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 380;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 3;
        UWORD row_h   = font_h + 4;
        UWORD lbl_w   = 100;
        UWORD gad_x   = bor_l + lbl_w;
        UWORD gad_w   = inner_w - lbl_w - pad;
        UWORD win_h   = bor_t + pad
                      + (UWORD)ADLG_ROWS * (row_h + pad)
                      + row_h + pad + bor_b;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct Gadget *prev = gctx;
            UWORD row = 0;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

#define ROW_Y(r) ((WORD)(bor_t + pad + (r) * (row_h + pad)))
#define STR_GAD(gid, lbl, initstr, maxch, pgad) \
    ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row); \
    ng.ng_Width=gad_w; ng.ng_Height=row_h; \
    ng.ng_GadgetText=(lbl); ng.ng_GadgetID=(gid); ng.ng_Flags=PLACETEXT_LEFT; \
    { struct TagItem st_[]={{GTST_String,(ULONG)(initstr)}, \
                            {GTST_MaxChars,(maxch)},{TAG_DONE,0}}; \
      *(pgad)=CreateGadgetA(STRING_KIND,prev,&ng,st_); \
      if (!*(pgad)) goto cleanup; prev=*(pgad); } row++;

            STR_GAD(ADLG_BUFFERS,     "Buffers",     buffers_str,  6,  &buffers_gad)

            /* BufMemType cycle */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Buf Mem Type"; ng.ng_GadgetID=ADLG_BUFMEMTYPE;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem bmt[]={{GTCY_Labels,(ULONG)bufmem_labels},
                                    {GTCY_Active,(ULONG)cur_bufmem},{TAG_DONE,0}};
              prev=CreateGadgetA(CYCLE_KIND,prev,&ng,bmt);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(ADLG_BOOTBLOCKS,  "Boot Blocks", bootblks_str, 6,  &bootblks_gad)
            STR_GAD(ADLG_MAXTRANSFER, "MaxTransfer", maxtrans_str, 12, &maxtrans_gad)
            STR_GAD(ADLG_MASK,        "Mask",        mask_str,     12, &mask_gad)

#undef STR_GAD
#undef ROW_Y

            /* OK / Cancel */
            {
                UWORD btn_y  = bor_t + pad + (UWORD)ADLG_ROWS * (row_h + pad);
                UWORD half_w = (inner_w - pad * 2 - pad) / 2;
                struct TagItem bt[] = { { TAG_DONE, 0 } };
                ng.ng_TopEdge=btn_y; ng.ng_Height=row_h;
                ng.ng_Width=half_w; ng.ng_Flags=PLACETEXT_IN;
                ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="OK";
                ng.ng_GadgetID=ADLG_OK;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+half_w+pad;
                ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=ADLG_CANCEL;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
                if (!prev) goto cleanup;
            }
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width -win_w)/2) },
                { WA_Top,       (ULONG)((scr->Height-win_h)/2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)"Advanced Partition Settings" },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                                WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;
    GT_RefreshWindow(win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case ADLG_BUFMEMTYPE: cur_bufmem = (UWORD)code; break;
                    case ADLG_OK: {
                        struct StringInfo *si;
                        si = (struct StringInfo *)buffers_gad->SpecialInfo;
                        pi->num_buffer   = parse_num((char *)si->Buffer);
                        pi->buf_mem_type = bufmem_values[cur_bufmem];
                        si = (struct StringInfo *)bootblks_gad->SpecialInfo;
                        pi->boot_blocks  = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)maxtrans_gad->SpecialInfo;
                        pi->max_transfer = parse_num((char *)si->Buffer);
                        si = (struct StringInfo *)mask_gad->SpecialInfo;
                        pi->mask         = parse_num((char *)si->Buffer);
                        running = FALSE; break;
                    }
                    case ADLG_CANCEL: running = FALSE; break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
}

/* ------------------------------------------------------------------ */
/* Main partition add / edit dialog                                    */
/* ------------------------------------------------------------------ */

static BOOL partition_dialog(struct PartInfo *pi, const char *title,
                             const struct RDBInfo *rdb)
{
    struct Screen  *scr          = NULL;
    APTR            vi           = NULL;
    struct Gadget  *glist        = NULL;
    struct Gadget  *gctx         = NULL;
    struct Gadget  *name_gad     = NULL;
    struct Gadget  *sizemb_gad   = NULL;
    struct Gadget  *bootpri_gad  = NULL;
    struct Gadget  *boot_gad     = NULL;
    struct Gadget  *dirscsi_gad  = NULL;
    struct Window  *win          = NULL;
    BOOL            result       = FALSE;
    UWORD           cur_fs       = 1;   /* default FFS */

    /* Dynamic filesystem list: built-ins + whatever is in the RDB FSHD list */
#define MAX_DLG_FS (NUM_BUILTIN_FS + MAX_FILESYSTEMS + 1)
    char       dlg_fs_names[MAX_DLG_FS][16];
    const char *dlg_fs_labels[MAX_DLG_FS + 1];
    ULONG       dlg_fs_dostypes[MAX_DLG_FS];
    UWORD       dlg_num_fs = 0;
    {
        UWORD bi, fi, k;
        /* Built-in filesystems */
        for (bi = 0; bi < NUM_BUILTIN_FS; bi++) {
            dlg_fs_labels[dlg_num_fs]   = builtin_fs[bi].name;
            dlg_fs_dostypes[dlg_num_fs] = builtin_fs[bi].dostype;
            dlg_num_fs++;
        }
        /* Add filesystems present in the RDB FSHD list */
        for (fi = 0; fi < rdb->num_fs; fi++) {
            ULONG dt = rdb->filesystems[fi].dos_type;
            BOOL dup = FALSE;
            for (k = 0; k < dlg_num_fs; k++)
                if (dlg_fs_dostypes[k] == dt) { dup = TRUE; break; }
            if (!dup && dlg_num_fs < MAX_DLG_FS - 1) {
                FormatDosType(dt, dlg_fs_names[dlg_num_fs]);
                dlg_fs_labels[dlg_num_fs]   = dlg_fs_names[dlg_num_fs];
                dlg_fs_dostypes[dlg_num_fs] = dt;
                dlg_num_fs++;
            }
        }
        /* Ensure the partition's current dos_type is in the list */
        {
            BOOL dup = FALSE;
            for (k = 0; k < dlg_num_fs; k++)
                if (dlg_fs_dostypes[k] == pi->dos_type) { dup = TRUE; break; }
            if (!dup && dlg_num_fs < MAX_DLG_FS - 1) {
                FormatDosType(pi->dos_type, dlg_fs_names[dlg_num_fs]);
                dlg_fs_labels[dlg_num_fs]   = dlg_fs_names[dlg_num_fs];
                dlg_fs_dostypes[dlg_num_fs] = pi->dos_type;
                dlg_num_fs++;
            }
        }
        dlg_fs_labels[dlg_num_fs] = NULL;
        /* Find index matching current dos_type */
        for (k = 0; k < dlg_num_fs; k++)
            if (dlg_fs_dostypes[k] == pi->dos_type) { cur_fs = k; break; }
    }

    char locyl_str[16], sizemb_str[16], bootpri_str[16];
    {
        ULONG bytes_per_cyl = pi->heads * pi->sectors * pi->block_size;
        ULONG cyl_count     = (pi->high_cyl >= pi->low_cyl)
                              ? (pi->high_cyl - pi->low_cyl + 1) : 1;
        UQUAD total_bytes   = (bytes_per_cyl > 0)
                              ? (UQUAD)cyl_count * bytes_per_cyl : 0;
        ULONG size_mb       = (ULONG)(total_bytes / (1024ULL * 1024ULL));
        if (size_mb == 0 && total_bytes > 0) size_mb = 1;
        sprintf(sizemb_str, "%lu", (unsigned long)size_mb);
    }
    sprintf(locyl_str,   "Lo: %lu", (unsigned long)pi->low_cyl);
    sprintf(bootpri_str, "%ld", (long)pi->boot_pri);

    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 380;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 3;
        UWORD row_h   = font_h + 4;
        UWORD lbl_w   = 100;
        UWORD gad_x   = bor_l + lbl_w;
        UWORD gad_w   = inner_w - lbl_w - pad;
        UWORD win_h   = bor_t + pad
                      + (UWORD)PDLG_ROWS * (row_h + pad)
                      + row_h + pad
                      + bor_b;

        gctx = CreateContext(&glist);
        if (!gctx) goto cleanup;

        {
            struct NewGadget ng;
            struct Gadget *prev;
            UWORD row = 0;
            memset(&ng, 0, sizeof(ng));
            ng.ng_VisualInfo = vi;
            ng.ng_TextAttr   = scr->Font;

#define ROW_Y(r) ((WORD)(bor_t + pad + (r) * (row_h + pad)))
#define STR_GAD(gid, lbl, initstr, maxch, pgad) \
    ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row); \
    ng.ng_Width=gad_w;    ng.ng_Height=row_h; \
    ng.ng_GadgetText=(lbl); ng.ng_GadgetID=(gid); ng.ng_Flags=PLACETEXT_LEFT; \
    { struct TagItem st_[]={ {GTST_String,(ULONG)(initstr)}, \
                             {GTST_MaxChars,(maxch)}, {TAG_DONE,0} }; \
      *(pgad)=CreateGadgetA(STRING_KIND, prev, &ng, st_); \
      if (!*(pgad)) goto cleanup; prev=*(pgad); } row++;

            /* Row 0: Name */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Name"; ng.ng_GadgetID=PDLG_NAME;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem st[]={{GTST_String,(ULONG)pi->drive_name},
                                   {GTST_MaxChars,30},{TAG_DONE,0}};
              name_gad=CreateGadgetA(STRING_KIND,gctx,&ng,st);
              if (!name_gad) goto cleanup; prev=name_gad; }
            row++;

            /* Lo Cylinder — reference display only, not editable */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="Cylinder"; ng.ng_GadgetID=0; ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem tt[]={{GTTX_Text,(ULONG)locyl_str},{TAG_DONE,0}};
              prev=CreateGadgetA(TEXT_KIND,prev,&ng,tt);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(PDLG_SIZEMB,  "Size (MB)",    sizemb_str,  10, &sizemb_gad)

            /* Filesystem (cycle) */
            ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=ROW_Y(row);
            ng.ng_Width=gad_w; ng.ng_Height=row_h;
            ng.ng_GadgetText="FileSystem"; ng.ng_GadgetID=PDLG_TYPE;
            ng.ng_Flags=PLACETEXT_LEFT;
            { struct TagItem ct[]={{GTCY_Labels,(ULONG)dlg_fs_labels},
                                   {GTCY_Active,(ULONG)cur_fs},{TAG_DONE,0}};
              prev=CreateGadgetA(CYCLE_KIND,prev,&ng,ct);
              if (!prev) goto cleanup; }
            row++;

            STR_GAD(PDLG_BOOTPRI, "Boot Priority", bootpri_str, 8, &bootpri_gad)

            /* Bootable [x]   Direct SCSI [x] */
            {
                BOOL is_bootable = (BOOL)((pi->flags & 2) == 0);
                BOOL is_dirscsi  = (BOOL)((pi->flags & 4) != 0);
                UWORD half = (inner_w - pad * 3) / 2;
                struct TagItem cbt[] = { { GTCB_Checked, 0 }, { TAG_DONE, 0 } };

                cbt[0].ti_Data = (ULONG)is_bootable;
                ng.ng_LeftEdge=bor_l+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Bootable"; ng.ng_GadgetID=PDLG_BOOTABLE;
                ng.ng_Flags=PLACETEXT_RIGHT;
                boot_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!boot_gad) goto cleanup; prev=boot_gad;

                cbt[0].ti_Data=(ULONG)is_dirscsi;
                ng.ng_LeftEdge=bor_l+pad+half+pad; ng.ng_TopEdge=ROW_Y(row);
                ng.ng_Width=half; ng.ng_Height=row_h;
                ng.ng_GadgetText="Direct SCSI"; ng.ng_GadgetID=PDLG_DIRSCSI;
                ng.ng_Flags=PLACETEXT_RIGHT;
                dirscsi_gad=CreateGadgetA(CHECKBOX_KIND,prev,&ng,cbt);
                if (!dirscsi_gad) goto cleanup; prev=dirscsi_gad;
            }
            row++;

#undef STR_GAD
#undef ROW_Y

            /* Three buttons: OK / Advanced... / Cancel */
            {
                UWORD btn_y  = bor_t + pad + (UWORD)PDLG_ROWS * (row_h + pad);
                UWORD third  = (inner_w - pad * 2 - pad * 2) / 3;
                struct TagItem bt[] = { { TAG_DONE, 0 } };
                ng.ng_TopEdge=btn_y; ng.ng_Height=row_h;
                ng.ng_Width=third; ng.ng_Flags=PLACETEXT_IN;
                ng.ng_LeftEdge=bor_l+pad;
                ng.ng_GadgetText="OK"; ng.ng_GadgetID=PDLG_OK;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+third+pad;
                ng.ng_GadgetText="Advanced..."; ng.ng_GadgetID=PDLG_ADVANCED;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
                ng.ng_LeftEdge=bor_l+pad+(third+pad)*2;
                ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=PDLG_CANCEL;
                prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if (!prev) goto cleanup;
            }
        }

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width -win_w)/2) },
                { WA_Top,       (ULONG)((scr->Height-win_h)/2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)title },
                { WA_Gadgets,   (ULONG)glist },
                { WA_PubScreen, (ULONG)scr },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW },
                { WA_Flags,     WFLG_DRAGBAR|WFLG_DEPTHGADGET|
                                WFLG_CLOSEGADGET|WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;

    GT_RefreshWindow(win, NULL);
    ActivateGadget(name_gad, win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case PDLG_TYPE: cur_fs = (UWORD)code; break;
                    case PDLG_ADVANCED:
                        partition_advanced_dialog(pi);
                        break;
                    case PDLG_OK: {
                        struct StringInfo *si;
                        si = (struct StringInfo *)name_gad->SpecialInfo;
                        strncpy(pi->drive_name, (char *)si->Buffer,
                                sizeof(pi->drive_name)-1);
                        /* Convert Size (MB) to high_cyl */
                        {
                            ULONG bytes_per_cyl = pi->heads * pi->sectors
                                                  * pi->block_size;
                            ULONG max_hi = rdb->hi_cyl;
                            UWORD k;
                            /* clamp to nearest occupied partition above lo_cyl */
                            for (k = 0; k < rdb->num_parts; k++) {
                                if (&rdb->parts[k] == pi) continue;
                                if (rdb->parts[k].low_cyl > pi->low_cyl &&
                                    rdb->parts[k].low_cyl - 1 < max_hi)
                                    max_hi = rdb->parts[k].low_cyl - 1;
                            }
                            si = (struct StringInfo *)sizemb_gad->SpecialInfo;
                            if (bytes_per_cyl > 0) {
                                ULONG size_mb    = parse_num((char *)si->Buffer);
                                UQUAD bytes_need = (UQUAD)size_mb * 1024ULL * 1024ULL;
                                ULONG cyls = (ULONG)((bytes_need + bytes_per_cyl - 1)
                                                     / bytes_per_cyl);
                                if (cyls == 0) cyls = 1;
                                pi->high_cyl = pi->low_cyl + cyls - 1;
                                if (pi->high_cyl > max_hi) pi->high_cyl = max_hi;
                            }
                        }
                        si = (struct StringInfo *)bootpri_gad->SpecialInfo;
                        pi->boot_pri = parse_long((char *)si->Buffer);
                        pi->flags = 0;
                        if (!(boot_gad->Flags    & GFLG_SELECTED)) pi->flags |= 2UL;
                        if (  dirscsi_gad->Flags  & GFLG_SELECTED)  pi->flags |= 4UL;
                        pi->dos_type = dlg_fs_dostypes[cur_fs];
                        result = TRUE; running = FALSE; break;
                    }
                    case PDLG_CANCEL: running = FALSE; break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}

/* ------------------------------------------------------------------ */
/* Filesystem manager dialog                                           */
/* ------------------------------------------------------------------ */

/* Gadget IDs for the filesystem manager window */
#define FSDLG_LIST    1
#define FSDLG_ADD     2
#define FSDLG_EDIT    3
#define FSDLG_DELETE  4
#define FSDLG_DONE    5

/* Gadget IDs for the add-FS sub-dialog */
#define AFSDLG_DOSTYPE  1
#define AFSDLG_FILE     2
#define AFSDLG_BROWSE   3
#define AFSDLG_OK       4
#define AFSDLG_CANCEL   5

static char        fs_strs[MAX_FILESYSTEMS][64];
static struct Node fs_nodes[MAX_FILESYSTEMS];
static struct List fs_list_gad;

static void build_fs_list(const struct RDBInfo *rdb)
{
    UWORD i;
    fs_list_gad.lh_Head     = (struct Node *)&fs_list_gad.lh_Tail;
    fs_list_gad.lh_Tail     = NULL;
    fs_list_gad.lh_TailPred = (struct Node *)&fs_list_gad.lh_Head;

    if (!rdb || !rdb->valid) return;
    for (i = 0; i < rdb->num_fs; i++) {
        const struct FSInfo *fi = &rdb->filesystems[i];
        char dt[16], ver[12], codesz[16];

        FormatDosType(fi->dos_type, dt);

        if (fi->version)
            sprintf(ver, "%lu.%lu",
                    (unsigned long)(fi->version >> 16),
                    (unsigned long)(fi->version & 0xFFFFUL));
        else
            sprintf(ver, "----");

        if (fi->code && fi->code_size > 0)
            FormatSize((UQUAD)fi->code_size, codesz);
        else
            sprintf(codesz, "No code");

        sprintf(fs_strs[i], "%-12s  %-8s  %s", dt, ver, codesz);

        fs_nodes[i].ln_Name = fs_strs[i];
        fs_nodes[i].ln_Type = NT_USER;
        fs_nodes[i].ln_Pri  = 0;
        AddTail(&fs_list_gad, &fs_nodes[i]);
    }
}

/* Load a file path into *fi->code/code_size. Shows error on failure.
   Returns TRUE on success (or if path is empty = no-code entry).     */
static BOOL fs_load_file(struct Window *win, const char *path, struct FSInfo *fi)
{
    struct EasyStruct es;
    BPTR fh;
    LONG fsize;
    UBYTE *code;

    if (path[0] == '\0') return TRUE;   /* no file = register DosType only */

    fh = Open((UBYTE *)path, MODE_OLDFILE);
    if (!fh) {
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"Cannot open file.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);
    if (fsize <= 0 || fsize >= (LONG)(1024L * 1024L)) {
        Close(fh);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"File is empty or too large\n(limit: 1 MB).";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    code = (UBYTE *)AllocVec((ULONG)fsize, MEMF_PUBLIC | MEMF_CLEAR);
    if (!code) { Close(fh); return FALSE; }
    if (Read(fh, code, fsize) != fsize) {
        Close(fh); FreeVec(code);
        es.es_StructSize=sizeof(es); es.es_Flags=0;
        es.es_Title=(UBYTE*)"Error";
        es.es_TextFormat=(UBYTE*)"File read error.";
        es.es_GadgetFormat=(UBYTE*)"OK";
        EasyRequest(win, &es, NULL);
        return FALSE;
    }
    Close(fh);
    if (fi->code) FreeVec(fi->code);
    fi->code      = code;
    fi->code_size = (ULONG)fsize;
    return TRUE;
}

/* Opens a sub-dialog to enter/edit DosType + optional file path.
   is_edit=FALSE: Add (default drawer L:), is_edit=TRUE: Edit.
   Returns TRUE if user confirmed, filling in *fi.               */
static BOOL fs_addedit_dialog(struct FSInfo *fi, BOOL is_edit)
{
    struct Screen *scr         = NULL;
    APTR           vi          = NULL;
    struct Gadget *glist       = NULL;
    struct Gadget *gctx        = NULL;
    struct Gadget *dostype_gad = NULL;
    struct Gadget *file_gad    = NULL;
    struct Window *win         = NULL;
    BOOL           result      = FALSE;
    char           dt_str[20];
    static char    file_str[256];   /* static so ASL path update persists */

    sprintf(dt_str, "0x%08lX", (unsigned long)fi->dos_type);
    if (is_edit) {
        file_str[0] = '\0';   /* empty = keep existing code */
    } else {
        strncpy(file_str, "L:", sizeof(file_str) - 1);
        file_str[sizeof(file_str) - 1] = '\0';
    }

    scr = LockPubScreen(NULL);
    if (!scr) goto fs_add_cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto fs_add_cleanup;

    {
        UWORD font_h   = scr->Font->ta_YSize;
        UWORD bor_l    = (UWORD)scr->WBorLeft;
        UWORD bor_t    = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r    = (UWORD)scr->WBorRight;
        UWORD bor_b    = (UWORD)scr->WBorBottom;
        UWORD win_w    = 460;
        UWORD inner_w  = win_w - bor_l - bor_r;
        UWORD pad      = 3;
        UWORD row_h    = font_h + 4;
        UWORD lbl_w    = 90;
        UWORD browse_w = 70;
        UWORD gad_x    = bor_l + lbl_w;
        UWORD file_w   = inner_w - lbl_w - browse_w - pad * 2;
        UWORD gad_w    = inner_w - lbl_w - pad;
        UWORD win_h    = bor_t + pad + row_h + pad + row_h + pad + row_h + pad + bor_b;
        struct NewGadget ng;
        struct Gadget *prev;

        gctx = CreateContext(&glist);
        if (!gctx) goto fs_add_cleanup;

        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;

        /* DosType */
        ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=(WORD)(bor_t+pad);
        ng.ng_Width=gad_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="DosType (hex or PFS\\3)"; ng.ng_GadgetID=AFSDLG_DOSTYPE;
        ng.ng_Flags=PLACETEXT_LEFT;
        { struct TagItem st[]={{GTST_String,(ULONG)dt_str},{GTST_MaxChars,18},{TAG_DONE,0}};
          dostype_gad=CreateGadgetA(STRING_KIND,gctx,&ng,st);
          if (!dostype_gad) goto fs_add_cleanup; prev=dostype_gad; }

        /* File string (narrower to leave room for Browse button) */
        ng.ng_LeftEdge=gad_x; ng.ng_TopEdge=(WORD)(bor_t+pad+row_h+pad);
        ng.ng_Width=file_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="File"; ng.ng_GadgetID=AFSDLG_FILE;
        ng.ng_Flags=PLACETEXT_LEFT;
        { struct TagItem st[]={{GTST_String,(ULONG)file_str},{GTST_MaxChars,255},{TAG_DONE,0}};
          file_gad=CreateGadgetA(STRING_KIND,prev,&ng,st);
          if (!file_gad) goto fs_add_cleanup; prev=file_gad; }

        /* Browse button — right of File string */
        ng.ng_LeftEdge=gad_x+(WORD)file_w+pad; ng.ng_TopEdge=(WORD)(bor_t+pad+row_h+pad);
        ng.ng_Width=browse_w; ng.ng_Height=row_h;
        ng.ng_GadgetText="Browse..."; ng.ng_GadgetID=AFSDLG_BROWSE;
        ng.ng_Flags=PLACETEXT_IN;
        { struct TagItem bt[]={{TAG_DONE,0}};
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt);
          if (!prev) goto fs_add_cleanup; }

        /* OK / Cancel */
        { UWORD btn_y = bor_t+pad+row_h+pad+row_h+pad;
          UWORD half  = (inner_w-pad*2-pad)/2;
          struct TagItem bt[]={{TAG_DONE,0}};
          ng.ng_TopEdge=btn_y; ng.ng_Height=row_h; ng.ng_Width=half;
          ng.ng_Flags=PLACETEXT_IN;
          ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="OK";
          ng.ng_GadgetID=AFSDLG_OK;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_add_cleanup;
          ng.ng_LeftEdge=bor_l+pad+half+pad;
          ng.ng_GadgetText="Cancel"; ng.ng_GadgetID=AFSDLG_CANCEL;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_add_cleanup; }

        { struct TagItem wt[]={
              {WA_Left,(ULONG)((scr->Width-win_w)/2)},
              {WA_Top,(ULONG)((scr->Height-win_h)/2)},
              {WA_Width,win_w},{WA_Height,win_h},
              {WA_Title,(ULONG)(is_edit ? "Edit FileSystem Driver" : "Add FileSystem Driver")},
              {WA_Gadgets,(ULONG)glist},{WA_PubScreen,(ULONG)scr},
              {WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|IDCMP_REFRESHWINDOW},
              {WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_CLOSEGADGET|
                        WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH},
              {TAG_DONE,0}};
          win=OpenWindowTagList(NULL,wt); }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto fs_add_cleanup;
    GT_RefreshWindow(win, NULL);
    ActivateGadget(dostype_gad, win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case AFSDLG_CANCEL: running = FALSE; break;

                    case AFSDLG_BROWSE:
                        if (AslBase) {
                            struct FileRequester *fr;
                            struct StringInfo *si = (struct StringInfo *)file_gad->SpecialInfo;
                            { struct TagItem asl_tags[] = {
                                  { ASLFR_TitleText,    (ULONG)"Select FileSystem Driver" },
                                  { ASLFR_InitialDrawer,(ULONG)"L:" },
                                  { ASLFR_InitialFile,  (ULONG)si->Buffer },
                                  { TAG_DONE, 0 } };
                              fr = (struct FileRequester *)AllocAslRequest(
                                  ASL_FileRequest, asl_tags); }
                            if (fr) {
                                if (AslRequest(fr, NULL)) {
                                    /* Build full path: drawer + file */
                                    strncpy(file_str, fr->fr_Drawer, sizeof(file_str)-1);
                                    file_str[sizeof(file_str)-1] = '\0';
                                    AddPart((UBYTE *)file_str, (UBYTE *)fr->fr_File,
                                            sizeof(file_str));
                                    /* Update the string gadget */
                                    { struct TagItem ut[]={{GTST_String,(ULONG)file_str},{TAG_DONE,0}};
                                      GT_SetGadgetAttrsA(file_gad, win, NULL, ut); }
                                }
                                FreeAslRequest(fr);
                            }
                        }
                        break;

                    case AFSDLG_OK: {
                        struct StringInfo *si;
                        si = (struct StringInfo *)dostype_gad->SpecialInfo;
                        fi->dos_type = parse_dostype((char *)si->Buffer);
                        si = (struct StringInfo *)file_gad->SpecialInfo;
                        if (fs_load_file(win, (char *)si->Buffer, fi))
                            result = TRUE;
                        if (result) running = FALSE;
                        break;
                    }
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

fs_add_cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return result;
}

/* Opens the filesystem manager window.
   Returns TRUE if any changes were made.                         */
static BOOL filesystem_manager_dialog(struct RDBInfo *rdb)
{
    struct Screen *scr    = NULL;
    APTR           vi     = NULL;
    struct Gadget *glist  = NULL;
    struct Gadget *gctx   = NULL;
    struct Gadget *lv_gad = NULL;
    struct Window *win    = NULL;
    BOOL           dirty  = FALSE;
    WORD           sel    = -1;

    build_fs_list(rdb);

    scr = LockPubScreen(NULL);
    if (!scr) goto fs_mgr_cleanup;
    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto fs_mgr_cleanup;

    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD bor_b   = (UWORD)scr->WBorBottom;
        UWORD win_w   = 480;
        UWORD inner_w = win_w - bor_l - bor_r;
        UWORD pad     = 4;
        UWORD btn_h   = font_h + 6;
        UWORD hdr_h   = font_h + 3;
        UWORD lv_h    = (UWORD)(font_h + 2) * 6;
        UWORD win_h   = bor_t + pad + hdr_h + lv_h + pad + btn_h + pad + bor_b;
        struct NewGadget ng;
        struct Gadget *prev;

        gctx = CreateContext(&glist);
        if (!gctx) goto fs_mgr_cleanup;

        memset(&ng, 0, sizeof(ng));
        ng.ng_VisualInfo = vi;
        ng.ng_TextAttr   = scr->Font;

        ng.ng_LeftEdge  = bor_l + pad;
        ng.ng_TopEdge   = (WORD)(bor_t + pad + hdr_h);
        ng.ng_Width     = inner_w - pad * 2;
        ng.ng_Height    = lv_h;
        ng.ng_GadgetID  = FSDLG_LIST;
        ng.ng_Flags     = 0;
        { struct TagItem lt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
          lv_gad=CreateGadgetA(LISTVIEW_KIND,gctx,&ng,lt);
          if (!lv_gad) goto fs_mgr_cleanup; }
        prev = lv_gad;

        { UWORD btn_y    = bor_t + pad + hdr_h + lv_h + pad;
          UWORD quarter  = (inner_w - pad * 2 - pad * 3) / 4;
          struct TagItem bt[] = {{TAG_DONE,0}};
          ng.ng_TopEdge=btn_y; ng.ng_Height=btn_h;
          ng.ng_Width=quarter; ng.ng_Flags=PLACETEXT_IN;
          ng.ng_LeftEdge=bor_l+pad; ng.ng_GadgetText="Add";
          ng.ng_GadgetID=FSDLG_ADD;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+quarter+pad; ng.ng_GadgetText="Edit";
          ng.ng_GadgetID=FSDLG_EDIT;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+(quarter+pad)*2; ng.ng_GadgetText="Delete";
          ng.ng_GadgetID=FSDLG_DELETE;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup;
          ng.ng_LeftEdge=bor_l+pad+(quarter+pad)*3; ng.ng_GadgetText="Done";
          ng.ng_GadgetID=FSDLG_DONE;
          prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); if(!prev) goto fs_mgr_cleanup; }

        { struct TagItem wt[]={
              {WA_Left,(ULONG)((scr->Width-win_w)/2)},
              {WA_Top,(ULONG)((scr->Height-win_h)/2)},
              {WA_Width,win_w},{WA_Height,win_h},
              {WA_Title,(ULONG)"FileSystem Drivers"},
              {WA_Gadgets,(ULONG)glist},{WA_PubScreen,(ULONG)scr},
              {WA_IDCMP,IDCMP_CLOSEWINDOW|IDCMP_GADGETUP|
                        IDCMP_GADGETDOWN|IDCMP_REFRESHWINDOW},
              {WA_Flags,WFLG_DRAGBAR|WFLG_DEPTHGADGET|WFLG_CLOSEGADGET|
                        WFLG_ACTIVATE|WFLG_SIMPLE_REFRESH},
              {TAG_DONE,0}};
          win=OpenWindowTagList(NULL,wt); }

        /* Column header drawn just above listview */
        if (win) {
            struct RastPort *rp = win->RPort;
            const char *hdr = "DosType       Version   Code";
            WORD fb = rp->TxBaseline;
            SetAPen(rp, 2); SetDrMd(rp, JAM2);
            RectFill(rp, bor_l+pad, bor_t+pad,
                     bor_l+pad+(WORD)(inner_w-pad*2)-1, (WORD)(bor_t+pad+hdr_h)-1);
            SetAPen(rp, 1); SetDrMd(rp, JAM1);
            Move(rp, bor_l+pad+4, (WORD)(bor_t+pad)+fb);
            Text(rp, hdr, strlen(hdr));
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto fs_mgr_cleanup;
    GT_RefreshWindow(win, NULL);

    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG iclass = imsg->Class;
                UWORD code   = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);
                switch (iclass) {
                case IDCMP_CLOSEWINDOW: running = FALSE; break;
                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == FSDLG_LIST) sel = (WORD)code;
                    break;
                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case FSDLG_LIST: sel = (WORD)code; break;
                    case FSDLG_DONE: running = FALSE; break;

                    case FSDLG_ADD:
                        if (rdb->num_fs < MAX_FILESYSTEMS) {
                            struct FSInfo new_fi;
                            memset(&new_fi, 0, sizeof(new_fi));
                            new_fi.dos_type   = 0x444F5301UL;  /* FFS default */
                            new_fi.version    = 0;
                            new_fi.priority   = 5;
                            new_fi.global_vec = -1L;
                            if (fs_addedit_dialog(&new_fi, FALSE)) {
                                rdb->filesystems[rdb->num_fs++] = new_fi;
                                dirty = TRUE;
                                { struct TagItem dt[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                                sel = (WORD)(rdb->num_fs - 1);
                            }
                        }
                        break;

                    case FSDLG_EDIT:
                        if (sel >= 0 && sel < (WORD)rdb->num_fs) {
                            /* Work on a copy so Cancel leaves original intact.
                               The copy starts with the existing code pointer; if
                               the user loads a new file fs_addedit_dialog frees
                               the old allocation via FreeVec before setting a new
                               one, so we must NOT free it again on cancel. */
                            struct FSInfo edit_fi = rdb->filesystems[sel];
                            if (fs_addedit_dialog(&edit_fi, TRUE)) {
                                rdb->filesystems[sel] = edit_fi;
                                dirty = TRUE;
                                { struct TagItem dt[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                            } else {
                                /* Cancelled: if fs_addedit_dialog loaded a new
                                   code buffer it stored it in edit_fi.code.
                                   Since we're not applying, free it if it differs
                                   from the original. */
                                if (edit_fi.code &&
                                    edit_fi.code != rdb->filesystems[sel].code)
                                    FreeVec(edit_fi.code);
                            }
                        }
                        break;

                    case FSDLG_DELETE:
                        if (sel >= 0 && sel < (WORD)rdb->num_fs) {
                            struct EasyStruct es;
                            char msg[64];
                            char dt[16];
                            FormatDosType(rdb->filesystems[sel].dos_type, dt);
                            sprintf(msg, "Delete filesystem driver %s?", dt);
                            es.es_StructSize  =sizeof(es);
                            es.es_Flags       =0;
                            es.es_Title       =(UBYTE*)"Delete FS Driver";
                            es.es_TextFormat  =(UBYTE*)msg;
                            es.es_GadgetFormat=(UBYTE*)"Yes|No";
                            if (EasyRequest(win, &es, NULL) == 1) {
                                UWORD j;
                                if (rdb->filesystems[sel].code)
                                    FreeVec(rdb->filesystems[sel].code);
                                for (j=(UWORD)sel; j+1 < rdb->num_fs; j++)
                                    rdb->filesystems[j] = rdb->filesystems[j+1];
                                rdb->num_fs--;
                                dirty = TRUE;
                                if (sel >= (WORD)rdb->num_fs)
                                    sel = (WORD)rdb->num_fs - 1;
                                { struct TagItem dt2[]={{GTLV_Labels,TAG_IGNORE},{TAG_DONE,0}};
                                  struct TagItem rt[]={{GTLV_Labels,(ULONG)&fs_list_gad},{TAG_DONE,0}};
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, dt2);
                                  build_fs_list(rdb);
                                  GT_SetGadgetAttrsA(lv_gad, win, NULL, rt); }
                            }
                        }
                        break;
                    }
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
                }
            }
        }
    }

fs_mgr_cleanup:
    if (win)   { RemoveGList(win, glist, -1); CloseWindow(win); }
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    return dirty;
}

/* ------------------------------------------------------------------ */
/* Hit-test: which partition block contains map x-coordinate           */
/* Returns partition index, or -1 if none.                             */
/* ------------------------------------------------------------------ */

static WORD hit_test_partition(const struct RDBInfo *rdb,
                                WORD mx, UWORD mw, ULONG total,
                                WORD mouse_x)
{
    UWORD i;
    for (i = 0; i < rdb->num_parts; i++) {
        WORD lx = (WORD)(mx + (WORD)((UQUAD)rdb->parts[i].low_cyl      * mw / total));
        WORD rx = (WORD)(mx + (WORD)((UQUAD)(rdb->parts[i].high_cyl+1) * mw / total));
        if (mouse_x >= lx && mouse_x < rx) return (WORD)i;
    }
    return -1;
}

/* Hit-test: find which partition edge is at map x-coordinate          */
/*                                                                     */
/* Returns partition index and sets *edge_out (0=left, 1=right),      */
/* or -1 if no edge within tolerance.                                  */
/* mx = map inner left, mw = map inner width, total = hi_cyl+1        */
/* ------------------------------------------------------------------ */

#define DRAG_TOL 5   /* pixel tolerance for edge hit */

static WORD hit_test_edge(const struct RDBInfo *rdb,
                           WORD mx, UWORD mw, ULONG total,
                           WORD mouse_x, WORD *edge_out)
{
    UWORD i;
    for (i = 0; i < rdb->num_parts; i++) {
        WORD lx = (WORD)(mx + (WORD)((UQUAD)rdb->parts[i].low_cyl  * mw / total));
        WORD rx = (WORD)(mx + (WORD)((UQUAD)(rdb->parts[i].high_cyl+1) * mw / total));
        WORD dl = mouse_x - lx; if (dl < 0) dl = -dl;
        WORD dr = mouse_x - rx; if (dr < 0) dr = -dr;
        if (dl <= DRAG_TOL) { *edge_out = 0; return (WORD)i; }
        if (dr <= DRAG_TOL) { *edge_out = 1; return (WORD)i; }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Main window layout — extracted so it can be rebuilt on resize       */
/* ------------------------------------------------------------------ */

struct PartLayout {
    WORD  ix, iy; UWORD iw;
    WORD  bx, by; UWORD bw, bh;
    WORD  hx, hy; UWORD hw;
};

/*
 * Build (or rebuild) the main window gadget list from current window dimensions.
 * vi must remain valid for the gadgets' lifetime.
 * Returns TRUE on success; on failure, frees any partial gadget list internally.
 */
static BOOL build_gadgets(APTR vi,
                           UWORD win_w, UWORD win_h,
                           UWORD bor_l, UWORD bor_t,
                           UWORD bor_r, UWORD bor_b,
                           UWORD font_h,
                           struct TextAttr *font_ta,
                           struct Gadget **out_glist,
                           struct Gadget **out_lv_gad,
                           struct PartLayout *lay)
{
    struct Gadget  *gctx = NULL, *glist = NULL, *lv = NULL, *prev;
    struct NewGadget ng;
    struct TagItem   bt[] = { { TAG_DONE, 0 } };
    UWORD inner_w = win_w - bor_l - bor_r;
    UWORD pad     = 4;
    UWORD info_h  = font_h * 2 + 6;
    UWORD map_h   = 40;
    UWORD lbl_h   = font_h + 4;
    UWORD hdr_h   = font_h + 3;
    UWORD btn_h   = font_h + 6;
    UWORD row_h   = font_h + 2;
    /* Buttons anchored to the bottom; listview fills remaining space */
    UWORD btn_y   = win_h - bor_b - pad - btn_h;
    UWORD lv_top;
    UWORD lv_h;
    UWORD seventh;

    lay->ix = (WORD)(bor_l + pad);
    lay->iy = (WORD)(bor_t + pad);
    lay->iw = inner_w - pad * 2;
    lay->bx = (WORD)(bor_l + pad);
    lay->by = (WORD)(bor_t + pad + info_h + pad);
    lay->bw = inner_w - pad * 2;
    lay->bh = map_h;
    lay->hx = (WORD)(bor_l + pad);
    lay->hy = (WORD)(bor_t + pad + info_h + pad + map_h + lbl_h + pad);
    lay->hw = inner_w - pad * 2;

    lv_top = (UWORD)(lay->hy + (WORD)hdr_h);
    lv_h   = (btn_y > lv_top + pad + row_h * 2)
             ? (btn_y - pad - lv_top) : row_h * 2;
    lv_h   = (lv_h / row_h) * row_h;   /* snap to whole rows */

    gctx = CreateContext(&glist);
    if (!gctx) return FALSE;

    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = font_ta;

    /* Partition listview */
    {
        struct TagItem lt[] = {
            { GTLV_Labels, (ULONG)&part_list }, { TAG_DONE, 0 }
        };
        ng.ng_LeftEdge   = bor_l + pad;
        ng.ng_TopEdge    = (WORD)lv_top;
        ng.ng_Width      = inner_w - pad * 2;
        ng.ng_Height     = lv_h;
        ng.ng_GadgetText = NULL;
        ng.ng_GadgetID   = GID_PARTLIST;
        ng.ng_Flags      = 0;
        lv = CreateGadgetA(LISTVIEW_KIND, gctx, &ng, lt);
        if (!lv) { FreeGadgets(glist); return FALSE; }
    }
    prev = lv;

    /* Button row */
    seventh = (inner_w - pad * 2 - pad * 6) / 7;
    ng.ng_TopEdge = (WORD)btn_y;
    ng.ng_Height  = btn_h;
    ng.ng_Width   = seventh;

#define MKBTN(lx,txt,gid) \
    ng.ng_LeftEdge=(lx); ng.ng_GadgetText=(txt); ng.ng_GadgetID=(gid); \
    prev=CreateGadgetA(BUTTON_KIND,prev,&ng,bt); \
    if (!prev) { FreeGadgets(glist); return FALSE; }

    MKBTN(bor_l+pad,                      "Init RDB", GID_INITRDB)
    MKBTN(bor_l+pad+(seventh+pad)*1,      "Add",      GID_ADD)
    MKBTN(bor_l+pad+(seventh+pad)*2,      "Edit",     GID_EDIT)
    MKBTN(bor_l+pad+(seventh+pad)*3,      "Delete",   GID_DELETE)
    MKBTN(bor_l+pad+(seventh+pad)*4,      "FileSys",  GID_FILESYS)
    MKBTN(bor_l+pad+(seventh+pad)*5,      "Write",    GID_WRITE)
    MKBTN(bor_l+pad+(seventh+pad)*6,      "Back",     GID_BACK)
#undef MKBTN

    *out_glist  = glist;
    *out_lv_gad = lv;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* partview_run                                                         */
/* ------------------------------------------------------------------ */

static void show_about(struct Window *win)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)"About DiskPart";
    es.es_TextFormat   = (UBYTE *)
        "DiskPart\n"
        "AmigaOS 3.x RDB Hard Disk Partition Editor\n"
        "\n"
        "A native GadTools application with full RDB support.\n"
        "No external library dependencies beyond the ROM.\n"
        "\n"
        "Director: John Hertell\n"
        "Code: Claude Code (Anthropic)\n"
        "\n"
        "https://github.com/ChuckyGang/DiskPart\n"
        "\n"
        "MIT License \xa9 2026 John Hertell";
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

static struct NewMenu partview_menu_def[] = {
    { NM_TITLE, "DiskPart", NULL, 0, 0, NULL },
    { NM_ITEM,  "About...", NULL, 0, 0, NULL },
    { NM_END,   NULL,       NULL, 0, 0, NULL },
};

BOOL partview_run(const char *devname, ULONG unit)
{
    struct BlockDev  *bd       = NULL;
    struct RDBInfo   *rdb      = NULL;
    struct Screen    *scr      = NULL;
    APTR              vi       = NULL;
    struct Gadget    *glist    = NULL;
    struct Gadget    *lv_gad   = NULL;
    struct Window    *win      = NULL;
    struct Menu      *menu     = NULL;
    WORD              sel      = -1;
    BOOL              dirty    = FALSE;   /* unsaved changes pending */
    BOOL              exit_req = FALSE;
    WORD              i;
    static char       win_title[80];

    /* Drag resize state */
    WORD  drag_part    = -1;   /* -1 = not dragging */
    WORD  drag_edge    = 0;    /* 0 = left (low_cyl), 1 = right (high_cyl) */
    ULONG drag_min     = 0;
    ULONG drag_max     = 0;
    ULONG drag_orig_lo = 0;   /* saved low_cyl  before drag */
    ULONG drag_orig_hi = 0;   /* saved high_cyl before drag */

    /* Double-click detection in map */
    ULONG dbl_sec   = 0;
    ULONG dbl_mic   = 0;
    WORD  dbl_part  = -1;   /* partition clicked last time */

    /* New-partition drag state */
    BOOL  drag_new       = FALSE;
    ULONG drag_new_lo    = 0;
    ULONG drag_new_hi    = 0;
    ULONG drag_new_start = 0;
    ULONG drag_new_min   = 0;   /* free-space left boundary */
    ULONG drag_new_max   = 0;   /* free-space right boundary */

    /* Layout coordinates filled in below */
    WORD  ix = 0, iy = 0;  UWORD iw = 0;           /* info section  */
    WORD  bx = 0, by = 0;  UWORD bw = 0, bh = 0;   /* map           */
    WORD  hx = 0, hy = 0;  UWORD hw = 0;            /* col header    */

    for (i = 0; i < NUM_PART_COLORS; i++) part_pens[i] = -1;
    bg_pen = rdb_pen = -1;

    /* ---- Open device, read RDB, get geometry if needed ---- */
    bd = BlockDev_Open(devname, unit);

    rdb = (struct RDBInfo *)AllocVec(sizeof(*rdb), MEMF_PUBLIC | MEMF_CLEAR);
    if (!rdb) goto cleanup;

    if (bd) {
        RDB_Read(bd, rdb);
        if (!rdb->valid) {
            struct DriveGeometry geom;
            memset(&geom, 0, sizeof(geom));
            bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
            bd->iotd.iotd_Req.io_Length  = sizeof(geom);
            bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
            bd->iotd.iotd_Req.io_Flags   = 0;
            if (DoIO((struct IORequest *)&bd->iotd) == 0 && geom.dg_Cylinders > 0) {
                rdb->cylinders = geom.dg_Cylinders;
                rdb->heads     = geom.dg_Heads;
                rdb->sectors   = geom.dg_TrackSectors;
            }
        }
    }

    build_part_list(rdb, sel);

    /* ---- Lock screen ---- */
    scr = LockPubScreen(NULL);
    if (!scr) goto cleanup;

    vi = GetVisualInfoA(scr, NULL);
    if (!vi) goto cleanup;

    alloc_pens(scr);

    /* ---- Open window first (no gadgets) to learn the actual border sizes.
            WFLG_SIZEBBOTTOM expands BorderBottom beyond scr->WBorBottom, so
            we cannot compute correct gadget positions until after open. ---- */
    {
        UWORD font_h  = scr->Font->ta_YSize;
        UWORD bor_l   = (UWORD)scr->WBorLeft;
        UWORD bor_t   = (UWORD)scr->WBorTop + font_h + 1;
        UWORD bor_r   = (UWORD)scr->WBorRight;
        UWORD pad     = 4;
        UWORD info_h  = font_h * 2 + 6;
        UWORD map_h   = 40;
        UWORD lbl_h   = font_h + 4;
        UWORD hdr_h   = font_h + 3;
        UWORD btn_h   = font_h + 6;
        UWORD row_h   = font_h + 2;
        UWORD win_w   = 560;
        /* Estimate bottom border generously: scr->WBorBottom + font height + a few
           pixels for the size gadget.  Overestimating just gives extra listview
           rows; underestimating would clip the buttons. */
        UWORD bor_b_est = (UWORD)scr->WBorBottom + font_h + 4;
        UWORD fixed_est = bor_t + pad + info_h + pad + map_h + lbl_h
                        + pad + hdr_h + pad + btn_h + pad + bor_b_est;
        UWORD win_h   = fixed_est + row_h * 8;
        UWORD min_w   = bor_l + bor_r + pad * 2 + 7 * (40 + pad) - pad;
        UWORD min_h   = fixed_est + row_h * 2;

        sprintf(win_title, "DiskPart - %s unit %lu", devname, (unsigned long)unit);

        {
            struct TagItem wt[] = {
                { WA_Left,      (ULONG)((scr->Width  - win_w) / 2) },
                { WA_Top,       (ULONG)((scr->Height - win_h) / 2) },
                { WA_Width,     win_w }, { WA_Height, win_h },
                { WA_Title,     (ULONG)win_title },
                { WA_Gadgets,   NULL },          /* added after open, see below */
                { WA_PubScreen, (ULONG)scr },
                { WA_MinWidth,  min_w },
                { WA_MinHeight, min_h },
                { WA_MaxWidth,  (ULONG)scr->Width  },
                { WA_MaxHeight, (ULONG)scr->Height },
                { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                                IDCMP_GADGETDOWN  | IDCMP_REFRESHWINDOW |
                                IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE |
                                IDCMP_NEWSIZE | IDCMP_MENUPICK },
                { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                                WFLG_CLOSEGADGET | WFLG_ACTIVATE |
                                WFLG_SIMPLE_REFRESH | WFLG_REPORTMOUSE |
                                WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM },
                { TAG_DONE, 0 }
            };
            win = OpenWindowTagList(NULL, wt);
        }
    }

    UnlockPubScreen(NULL, scr); scr = NULL;
    if (!win) goto cleanup;

    /* ---- Build gadgets from the window's actual border sizes ---- */
    {
        struct PartLayout lay;
        UWORD fh = (UWORD)win->WScreen->Font->ta_YSize;

        if (!build_gadgets(vi,
                           (UWORD)win->Width,       (UWORD)win->Height,
                           (UWORD)win->BorderLeft,  (UWORD)win->BorderTop,
                           (UWORD)win->BorderRight, (UWORD)win->BorderBottom,
                           fh, win->WScreen->Font, &glist, &lv_gad, &lay))
            goto cleanup;

        ix = lay.ix; iy = lay.iy; iw = lay.iw;
        bx = lay.bx; by = lay.by; bw = lay.bw; bh = lay.bh;
        hx = lay.hx; hy = lay.hy; hw = lay.hw;

        AddGList(win, glist, (UWORD)-1, -1, NULL);
        RefreshGList(glist, win, NULL, -1);

        /* Now that we know the real border sizes, set precise size limits.
           The estimate used for WA_MinHeight at open time may have been off
           because WFLG_SIZEBBOTTOM enlarges BorderBottom unpredictably. */
        {
            UWORD pad2     = 4;
            UWORD info_h2  = fh * 2 + 6;
            UWORD map_h2   = 40;
            UWORD lbl_h2   = fh + 4;
            UWORD hdr_h2   = fh + 3;
            UWORD btn_h2   = fh + 6;
            UWORD row_h2   = fh + 2;
            UWORD fixed2   = (UWORD)win->BorderTop
                           + pad2 + info_h2 + pad2 + map_h2 + lbl_h2
                           + pad2 + hdr_h2  + pad2 + btn_h2
                           + pad2 + (UWORD)win->BorderBottom;
            WORD  min_h2   = (WORD)(fixed2 + row_h2 * 2);
            WORD  min_w2   = (WORD)((UWORD)win->BorderLeft + (UWORD)win->BorderRight
                                    + pad2 * 2 + 7 * (40 + pad2) - pad2);
            WindowLimits(win, min_w2, (WORD)win->WScreen->Width,
                              min_h2, (WORD)win->WScreen->Height);
        }
    }

    {
        struct TagItem lt[] = { { TAG_DONE, 0 } };
        menu = CreateMenusA(partview_menu_def, NULL);
        if (menu) {
            LayoutMenusA(menu, vi, lt);
            SetMenuStrip(win, menu);
        }
    }

    GT_RefreshWindow(win, NULL);
    draw_static(win, devname, unit, rdb,
                ix, iy, iw, bx, by, bw, bh, hx, hy, hw, sel);

    /* ---- Event loop ---- */
    {
        BOOL running = TRUE;
        while (running) {
            struct IntuiMessage *imsg;
            WaitPort(win->UserPort);
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG          iclass  = imsg->Class;
                UWORD          code    = imsg->Code;
                WORD           mouse_x = imsg->MouseX;
                WORD           mouse_y = imsg->MouseY;
                ULONG          ev_sec  = imsg->Seconds;
                ULONG          ev_mic  = imsg->Micros;
                struct Gadget *gad     = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (iclass) {

                case IDCMP_MENUPICK: {
                    UWORD mcode = code;
                    while (mcode != MENUNULL) {
                        struct MenuItem *it = ItemAddress(menu, mcode);
                        if (!it) break;
                        if (MENUNUM(mcode) == 0 && ITEMNUM(mcode) == 0)
                            show_about(win);
                        mcode = it->NextSelect;
                    }
                    break;
                }

                case IDCMP_CLOSEWINDOW: {
                    struct EasyStruct es;
                    es.es_StructSize   = sizeof(es);
                    es.es_Flags        = 0;
                    es.es_Title        = (UBYTE *)"DiskPart";
                    es.es_TextFormat   = (UBYTE *)"Exit DiskPart?";
                    es.es_GadgetFormat = (UBYTE *)"Yes|No";
                    if (EasyRequestArgs(win, &es, NULL, NULL) == 1) {
                        exit_req = TRUE;
                        running  = FALSE;
                    }
                    break;
                }

                case IDCMP_MOUSEBUTTONS:
                    if (code == SELECTDOWN) {
                        /* Hit-test partition edges/blocks in the map area */
                        if (rdb && rdb->valid &&
                            mouse_y >= by && mouse_y <= by + (WORD)bh)
                        {
                            WORD  mx2   = bx + 1;
                            UWORD mw2   = bw - 2;
                            ULONG total = rdb->hi_cyl + 1;
                            WORD  edge  = 0;
                            WORD  part  = hit_test_edge(rdb, mx2, mw2, total,
                                                         mouse_x, &edge);
                            if (part >= 0) {
                                /* On an edge — start drag, save originals */
                                ULONG left_end    = rdb->lo_cyl;   /* first usable cyl */
                                ULONG right_start = rdb->hi_cyl + 1;
                                UWORD kk;

                                /* Find nearest neighbours by cylinder, not array index */
                                for (kk = 0; kk < rdb->num_parts; kk++) {
                                    if (kk == (UWORD)part) continue;
                                    if (rdb->parts[kk].high_cyl < rdb->parts[part].low_cyl) {
                                        if (rdb->parts[kk].high_cyl + 1 > left_end)
                                            left_end = rdb->parts[kk].high_cyl + 1;
                                    }
                                    if (rdb->parts[kk].low_cyl > rdb->parts[part].high_cyl) {
                                        if (rdb->parts[kk].low_cyl < right_start)
                                            right_start = rdb->parts[kk].low_cyl;
                                    }
                                }

                                drag_part    = part;
                                drag_edge    = edge;
                                drag_orig_lo = rdb->parts[part].low_cyl;
                                drag_orig_hi = rdb->parts[part].high_cyl;
                                dbl_part     = -1;
                                if (edge == 0) {
                                    drag_min = left_end;
                                    drag_max = rdb->parts[part].high_cyl;
                                } else {
                                    drag_min = rdb->parts[part].low_cyl;
                                    drag_max = right_start > 0 ? right_start - 1 : 0;
                                }
                            } else {
                                /* Inside a partition block — check double-click */
                                WORD blk = hit_test_partition(rdb, mx2, mw2,
                                                               total, mouse_x);
                                if (blk >= 0) {
                                    if (blk == dbl_part &&
                                        DoubleClick(dbl_sec, dbl_mic,
                                                    ev_sec,  ev_mic)) {
                                        /* Double-click: open Edit dialog */
                                        sel = blk;
                                        sync_listview_sel(win, lv_gad, rdb, sel);
                                        dbl_part = -1;
                                        if (partition_dialog(&rdb->parts[sel],
                                                             "Edit Partition", rdb)) {
                                            dirty = TRUE;
                                            refresh_listview(win, lv_gad, rdb, sel);
                                            draw_static(win, devname, unit, rdb,
                                                        ix, iy, iw, bx, by, bw, bh,
                                                        hx, hy, hw, sel);
                                        }
                                    } else {
                                        /* Single click: select partition */
                                        sel      = blk;
                                        dbl_part = blk;
                                        dbl_sec  = ev_sec;
                                        dbl_mic  = ev_mic;
                                        sync_listview_sel(win, lv_gad, rdb, sel);
                                        draw_map(win, rdb, sel, bx, by, bw, bh);
                                    }
                                } else if (rdb->num_parts < MAX_PARTITIONS &&
                                           rdb->heads > 0 && rdb->sectors > 0) {
                                    /* Empty space — start new-partition drag */
                                    LONG  dx = (LONG)(mouse_x - (WORD)mx2);
                                    ULONG start_cyl;
                                    UWORD kk;
                                    if (dx < 0) dx = 0;
                                    if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                                    start_cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                                    if (start_cyl < rdb->lo_cyl) start_cyl = rdb->lo_cyl;
                                    if (start_cyl > rdb->hi_cyl) start_cyl = rdb->hi_cyl;
                                    /* Find free gap containing start_cyl */
                                    drag_new_min = rdb->lo_cyl;
                                    drag_new_max = rdb->hi_cyl;
                                    for (kk = 0; kk < rdb->num_parts; kk++) {
                                        if (rdb->parts[kk].high_cyl < start_cyl &&
                                            rdb->parts[kk].high_cyl + 1 > drag_new_min)
                                            drag_new_min = rdb->parts[kk].high_cyl + 1;
                                        if (rdb->parts[kk].low_cyl > start_cyl &&
                                            rdb->parts[kk].low_cyl - 1 < drag_new_max)
                                            drag_new_max = rdb->parts[kk].low_cyl - 1;
                                    }
                                    if (drag_new_min <= drag_new_max) {
                                        ULONG ini_hi = start_cyl;
                                        if (ini_hi < drag_new_min) ini_hi = drag_new_min;
                                        if (ini_hi > drag_new_max) ini_hi = drag_new_max;
                                        drag_new       = TRUE;
                                        drag_new_start = drag_new_min;  /* unused but keep tidy */
                                        drag_new_lo    = drag_new_min;
                                        drag_new_hi    = ini_hi;
                                        dbl_part       = -1;
                                        /* Show initial preview immediately */
                                        draw_map(win, rdb, sel, bx, by, bw, bh);
                                        draw_new_part_overlay(win, drag_new_lo, drag_new_hi,
                                                              rdb, bx, by, bw, bh);
                                    }
                                }
                            }
                        }
                    } else if (code == SELECTUP) {
                        if (drag_part >= 0) {
                            WORD  confirmed_part = drag_part;
                            drag_part = -1;

                            /* Only ask if something actually changed */
                            if (rdb->parts[confirmed_part].low_cyl  != drag_orig_lo ||
                                rdb->parts[confirmed_part].high_cyl != drag_orig_hi)
                            {
                                struct EasyStruct es;
                                char msg[128];
                                sprintf(msg,
                                    "Partition %s has been resized.\n"
                                    "Existing data may be lost!\n"
                                    "Keep this change?",
                                    rdb->parts[confirmed_part].drive_name);
                                es.es_StructSize   = sizeof(es);
                                es.es_Flags        = 0;
                                es.es_Title        = (UBYTE *)"Resize Partition";
                                es.es_TextFormat   = (UBYTE *)msg;
                                es.es_GadgetFormat = (UBYTE *)"Yes|No";
                                if (EasyRequest(win, &es, NULL) == 1) {
                                    dirty = TRUE;
                                } else {
                                    /* Revert */
                                    rdb->parts[confirmed_part].low_cyl  = drag_orig_lo;
                                    rdb->parts[confirmed_part].high_cyl = drag_orig_hi;
                                }
                            }
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb,
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel);
                        } else if (drag_new) {
                            drag_new = FALSE;
                            /* Open Add Partition dialog with dragged range */
                            {
                                struct PartInfo new_pi;
                                memset(&new_pi, 0, sizeof(new_pi));
                                sprintf(new_pi.drive_name, "DH%u",
                                        (unsigned)rdb->num_parts);
                                new_pi.dos_type     = 0x444F5301UL;   /* FFS */
                                new_pi.low_cyl      = drag_new_lo;
                                new_pi.high_cyl     = drag_new_hi;
                                new_pi.heads        = rdb->heads;
                                new_pi.sectors      = rdb->sectors;
                                new_pi.block_size   = 512;
                                new_pi.boot_pri     = (rdb->num_parts == 0) ? 0 : -128;
                                new_pi.max_transfer = 0x7FFFFFFFUL;
                                new_pi.mask         = 0xFFFFFFFEUL;
                                new_pi.num_buffer   = 30;
                                new_pi.buf_mem_type = 0;
                                new_pi.boot_blocks  = 2;
                                if (partition_dialog(&new_pi, "Add Partition", rdb)) {
                                    rdb->parts[rdb->num_parts++] = new_pi;
                                    dirty = TRUE;
                                    refresh_listview(win, lv_gad, rdb, sel);
                                }
                            }
                            draw_static(win, devname, unit, rdb,
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel);
                        }
                    }
                    break;

                case IDCMP_MOUSEMOVE:
                    if (drag_part >= 0 && rdb && rdb->valid) {
                        WORD  mx2   = bx + 1;
                        UWORD mw2   = bw - 2;
                        ULONG total = rdb->hi_cyl + 1;
                        LONG  dx    = (LONG)(mouse_x - (WORD)mx2);
                        ULONG new_cyl;

                        if (dx < 0)          dx = 0;
                        if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                        new_cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                        if (new_cyl < drag_min) new_cyl = drag_min;
                        if (new_cyl > drag_max) new_cyl = drag_max;

                        if (drag_edge == 0)
                            rdb->parts[drag_part].low_cyl  = new_cyl;
                        else
                            rdb->parts[drag_part].high_cyl = new_cyl;

                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        draw_drag_info(win, rdb, drag_part, bx, by, bw, bh);
                    } else if (drag_new && rdb && rdb->valid) {
                        WORD  mx2   = bx + 1;
                        UWORD mw2   = bw - 2;
                        ULONG total = rdb->hi_cyl + 1;
                        LONG  dx    = (LONG)(mouse_x - (WORD)mx2);
                        ULONG cyl;

                        if (dx < 0)          dx = 0;
                        if (dx >= (LONG)mw2) dx = (LONG)(mw2 - 1);
                        cyl = (ULONG)((UQUAD)(ULONG)dx * total / (ULONG)mw2);
                        if (cyl < drag_new_min) cyl = drag_new_min;
                        if (cyl > drag_new_max) cyl = drag_new_max;

                        /* lo is always anchored at the left of the free gap */
                        drag_new_lo = drag_new_min;
                        drag_new_hi = cyl;

                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        draw_new_part_overlay(win, drag_new_lo, drag_new_hi,
                                              rdb, bx, by, bw, bh);
                    }
                    break;

                case IDCMP_GADGETDOWN:
                    if (gad->GadgetID == GID_PARTLIST) {
                        sel = (WORD)code;
                        draw_map(win, rdb, sel, bx, by, bw, bh);
                    }
                    break;

                case IDCMP_GADGETUP:
                    switch (gad->GadgetID) {
                    case GID_PARTLIST:
                        sel = (WORD)code;
                        draw_map(win, rdb, sel, bx, by, bw, bh);
                        /* double-click → open Edit dialog */
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            if (partition_dialog(&rdb->parts[sel],
                                                 "Edit Partition", rdb)) {
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            }
                        }
                        break;

                    case GID_INITRDB: {
                        struct EasyStruct es;
                        struct DriveGeometry geom;
                        ULONG real_cyls = 0, real_heads = 0, real_secs = 0;

                        /* Always fetch actual geometry from the device */
                        if (bd) {
                            memset(&geom, 0, sizeof(geom));
                            bd->iotd.iotd_Req.io_Command = TD_GETGEOMETRY;
                            bd->iotd.iotd_Req.io_Length  = sizeof(geom);
                            bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
                            bd->iotd.iotd_Req.io_Flags   = 0;
                            if (DoIO((struct IORequest *)&bd->iotd) == 0 &&
                                geom.dg_Cylinders > 0) {
                                real_cyls  = geom.dg_Cylinders;
                                real_heads = geom.dg_Heads;
                                real_secs  = geom.dg_TrackSectors;
                            }
                        }
                        /* Fall back to whatever we already know */
                        if (real_cyls == 0) real_cyls  = rdb->cylinders;
                        if (real_cyls == 0) {
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"DiskPart";
                            es.es_TextFormat   = (UBYTE *)"Cannot determine disk geometry.\nMake sure the device is accessible.";
                            es.es_GadgetFormat = (UBYTE *)"OK";
                            EasyRequest(win, &es, NULL);
                            break;
                        }
                        if (real_heads == 0) real_heads = rdb->heads;
                        if (real_secs  == 0) real_secs  = rdb->sectors;

                        if (rdb->valid) {
                            /* Disk already has an RDB — offer Re-init or Update Geometry */
                            LONG choice;
                            char msg[512];

                            sprintf(msg,
                                "Disk already has an RDB with %u partition%s.\n\n"
                                "Device geometry: %lu cyl x %lu hd x %lu sec\n"
                                "RDB geometry:    %lu cyl x %lu hd x %lu sec\n\n"
                                "Re-init: wipe all partitions, create fresh RDB.\n"
                                "Update Geometry (EXPERIMENTAL): keep partitions,\n"
                                "  update RDB to match device size.",
                                (unsigned)rdb->num_parts,
                                rdb->num_parts == 1 ? "" : "s",
                                (unsigned long)real_cyls,
                                (unsigned long)real_heads,
                                (unsigned long)real_secs,
                                (unsigned long)rdb->cylinders,
                                (unsigned long)rdb->heads,
                                (unsigned long)rdb->sectors);

                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"Init RDB";
                            es.es_TextFormat   = (UBYTE *)msg;
                            es.es_GadgetFormat = (UBYTE *)"Re-init|Update Geometry|Cancel";
                            choice = EasyRequest(win, &es, NULL);

                            if (choice == 1) {
                                /* Re-init */
                                RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                sel   = -1;
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            } else if (choice == 2) {
                                /* Update Geometry (EXPERIMENTAL) */
                                rdb->cylinders = real_cyls;
                                rdb->heads     = real_heads;
                                rdb->sectors   = real_secs;
                                rdb->hi_cyl    = real_cyls - 1;
                                dirty = TRUE;
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            }
                        } else {
                            /* No RDB — confirm and create fresh */
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"Init RDB";
                            es.es_TextFormat   = (UBYTE *)"Create a new RDB on this disk?\nAll existing data will be lost.";
                            es.es_GadgetFormat = (UBYTE *)"Yes|No";
                            if (EasyRequest(win, &es, NULL) == 1) {
                                RDB_InitFresh(rdb, real_cyls, real_heads, real_secs);
                                sel   = -1;
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            }
                        }
                        break;
                    }

                    case GID_ADD: {
                        struct PartInfo new_pi;
                        ULONG lo, hi;
                        if (!rdb->valid) {
                            if (rdb->cylinders == 0) break;
                            RDB_InitFresh(rdb, rdb->cylinders,
                                          rdb->heads, rdb->sectors);
                        }
                        if (rdb->num_parts >= MAX_PARTITIONS) break;
                        find_free_range(rdb, &lo, &hi);
                        if (lo > hi) break;

                        memset(&new_pi, 0, sizeof(new_pi));
                        sprintf(new_pi.drive_name, "DH%u",
                                (unsigned)rdb->num_parts);
                        new_pi.dos_type     = 0x444F5301UL;   /* FFS */
                        new_pi.low_cyl      = lo;
                        new_pi.high_cyl     = hi;
                        new_pi.heads        = rdb->heads;
                        new_pi.sectors      = rdb->sectors;
                        new_pi.block_size   = 512;
                        new_pi.boot_pri     = (rdb->num_parts == 0) ? 0 : -128;
                        new_pi.max_transfer = 0x7FFFFFFFUL;
                        new_pi.mask         = 0xFFFFFFFEUL;
                        new_pi.num_buffer   = 30;
                        new_pi.buf_mem_type = 0;   /* Any */
                        new_pi.boot_blocks  = 2;
                        /* flags: 0 = bootable */

                        if (partition_dialog(&new_pi, "Add Partition", rdb)) {
                            rdb->parts[rdb->num_parts++] = new_pi;
                            dirty = TRUE;
                            refresh_listview(win, lv_gad, rdb, sel);
                            draw_static(win, devname, unit, rdb,
                                        ix, iy, iw, bx, by, bw, bh,
                                        hx, hy, hw, sel);
                        }
                        break;
                    }

                    case GID_EDIT:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            if (partition_dialog(&rdb->parts[sel],
                                                 "Edit Partition", rdb)) {
                                dirty = TRUE;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            }
                        }
                        break;

                    case GID_DELETE:
                        if (sel >= 0 && sel < (WORD)rdb->num_parts) {
                            struct EasyStruct es;
                            char msg[128];
                            sprintf(msg,
                                "Delete partition %s?\n"
                                "All data on this partition will be lost!",
                                rdb->parts[sel].drive_name);
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"Delete Partition";
                            es.es_TextFormat   = (UBYTE *)msg;
                            es.es_GadgetFormat = (UBYTE *)"Yes|No";
                            if (EasyRequest(win, &es, NULL) == 1) {
                                UWORD j;
                                for (j=(UWORD)sel; j+1 < rdb->num_parts; j++)
                                    rdb->parts[j] = rdb->parts[j+1];
                                rdb->num_parts--;
                                dirty = TRUE;
                                if (sel >= (WORD)rdb->num_parts)
                                    sel = (WORD)rdb->num_parts - 1;
                                refresh_listview(win, lv_gad, rdb, sel);
                                draw_static(win, devname, unit, rdb,
                                            ix, iy, iw, bx, by, bw, bh,
                                            hx, hy, hw, sel);
                            }
                        }
                        break;

                    case GID_FILESYS:
                        if (!rdb->valid) {
                            struct EasyStruct es;
                            es.es_StructSize   = sizeof(es);
                            es.es_Flags        = 0;
                            es.es_Title        = (UBYTE *)"DiskPart";
                            es.es_TextFormat   = (UBYTE *)"No RDB found.\nInit RDB first.";
                            es.es_GadgetFormat = (UBYTE *)"OK";
                            EasyRequest(win, &es, NULL);
                        } else {
                            if (filesystem_manager_dialog(rdb))
                                dirty = TRUE;
                        }
                        break;

                    case GID_WRITE: {
                        struct EasyStruct es;
                        es.es_StructSize   = sizeof(es);
                        es.es_Flags        = 0;
                        es.es_Title        = (UBYTE *)"DiskPart";
                        es.es_TextFormat   = (UBYTE *)"Write partition table to disk?\nAll existing data may be lost!";
                        es.es_GadgetFormat = (UBYTE *)"Write|Cancel";
                        if (EasyRequest(win, &es, NULL) == 1) {
                            if (bd) RDB_Write(bd, rdb);
                            dirty = FALSE;
                        }
                        break;
                    }

                    case GID_BACK:
                        running = FALSE; break;
                    }
                    break;

                case IDCMP_NEWSIZE: {
                    struct Gadget    *new_glist = NULL, *new_lv = NULL;
                    struct PartLayout new_lay;
                    UWORD fh = (UWORD)win->WScreen->Font->ta_YSize;

                    /* Cancel any in-progress drag */
                    drag_part = -1;

                    RemoveGList(win, glist, -1);
                    FreeGadgets(glist);
                    glist = NULL; lv_gad = NULL;

                    if (build_gadgets(vi,
                                      (UWORD)win->Width,  (UWORD)win->Height,
                                      (UWORD)win->BorderLeft,  (UWORD)win->BorderTop,
                                      (UWORD)win->BorderRight, (UWORD)win->BorderBottom,
                                      fh, win->WScreen->Font, &new_glist, &new_lv, &new_lay)) {
                        glist  = new_glist;
                        lv_gad = new_lv;
                        ix = new_lay.ix; iy = new_lay.iy; iw = new_lay.iw;
                        bx = new_lay.bx; by = new_lay.by;
                        bw = new_lay.bw; bh = new_lay.bh;
                        hx = new_lay.hx; hy = new_lay.hy; hw = new_lay.hw;

                        AddGList(win, glist, (UWORD)-1, -1, NULL);
                        RefreshGList(glist, win, NULL, -1);
                        GT_RefreshWindow(win, NULL);

                        /* Restore listview selection */
                        if (sel >= 0) {
                            struct TagItem st[] = {
                                { GTLV_Selected,    (ULONG)sel },
                                { GTLV_MakeVisible, (ULONG)sel },
                                { TAG_DONE, 0 }
                            };
                            GT_SetGadgetAttrsA(lv_gad, win, NULL, st);
                        }

                        draw_static(win, devname, unit, rdb,
                                    ix, iy, iw, bx, by, bw, bh,
                                    hx, hy, hw, sel);
                    }
                    break;
                }

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(win);
                    GT_EndRefresh(win, TRUE);
                    draw_static(win, devname, unit, rdb,
                                ix, iy, iw, bx, by, bw, bh,
                                hx, hy, hw, sel);
                    break;
                }
            }
        }
    }

cleanup:
    if (win)   { ClearMenuStrip(win); if (glist) RemoveGList(win, glist, -1); CloseWindow(win); }
    if (menu)    FreeMenus(menu);
    if (glist)   FreeGadgets(glist);
    if (vi)      FreeVisualInfo(vi);
    if (scr)     UnlockPubScreen(NULL, scr);
    {
        struct Screen *ws = LockPubScreen(NULL);
        if (ws) { free_pens(ws); UnlockPubScreen(NULL, ws); }
    }
    if (rdb)  { RDB_FreeCode(rdb); FreeVec(rdb); }
    if (bd)   BlockDev_Close(bd);
    return exit_req;
}
