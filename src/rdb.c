/*
 * rdb.c — Block device I/O and RDB read/write for DiskPart.
 *
 * Stage 1: BlockDev_Open / BlockDev_Close / BlockDev_ReadBlock /
 *          BlockDev_WriteBlock / BlockDev_HasMBR.
 * RDB_Read / RDB_Write / RDB_InitFresh added in later stages.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <dos/filehandler.h>
#include <proto/exec.h>

#include "clib.h"
#include "rdb.h"

/* ------------------------------------------------------------------ */
/* Local CreateMsgPort / DeleteMsgPort                                 */
/* (amiga.lib is not available in the ELF toolchain)                  */
/* ------------------------------------------------------------------ */

static struct MsgPort *local_create_port(void)
{
    struct MsgPort *port;
    BYTE sig = AllocSignal(-1);
    if (sig < 0) return NULL;

    port = (struct MsgPort *)AllocMem(sizeof(*port), MEMF_PUBLIC | MEMF_CLEAR);
    if (!port) { FreeSignal(sig); return NULL; }

    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags        = PA_SIGNAL;
    port->mp_SigBit       = (UBYTE)sig;
    port->mp_SigTask      = FindTask(NULL);

    port->mp_MsgList.lh_Head     = (struct Node *)&port->mp_MsgList.lh_Tail;
    port->mp_MsgList.lh_Tail     = NULL;
    port->mp_MsgList.lh_TailPred = (struct Node *)&port->mp_MsgList.lh_Head;

    return port;
}

static void local_delete_port(struct MsgPort *port)
{
    if (!port) return;
    FreeSignal((BYTE)port->mp_SigBit);
    FreeMem(port, sizeof(*port));
}

/* ------------------------------------------------------------------ */
/* BlockDev_Open                                                       */
/* ------------------------------------------------------------------ */

struct BlockDev *BlockDev_Open(const char *devname, ULONG unit)
{
    struct BlockDev *bd;
    struct DriveGeometry geom;
    LONG err;

    bd = (struct BlockDev *)AllocVec(sizeof(*bd), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bd) return NULL;

    strncpy(bd->devname, devname, sizeof(bd->devname) - 1);
    bd->unit = unit;

    bd->port = local_create_port();
    if (!bd->port) { FreeVec(bd); return NULL; }

    bd->iotd.iotd_Req.io_Message.mn_ReplyPort = bd->port;

    err = OpenDevice((UBYTE *)devname, unit,
                     (struct IORequest *)&bd->iotd, 0);
    if (err != 0) {
        local_delete_port(bd->port);
        FreeVec(bd);
        return NULL;
    }
    bd->open = TRUE;

    /* Try to read drive geometry for capacity info */
    bd->iotd.iotd_Req.io_Command = TD_GETDRIVETYPE;
    bd->iotd.iotd_Req.io_Length  = sizeof(geom);
    bd->iotd.iotd_Req.io_Data    = (APTR)&geom;
    bd->iotd.iotd_Req.io_Flags   = 0;
    DoIO((struct IORequest *)&bd->iotd);
    /* ignore error — geometry is informational only */

    bd->block_size  = 512;          /* safe default */
    bd->total_bytes = 0;

    return bd;
}

/* ------------------------------------------------------------------ */
/* BlockDev_Close                                                      */
/* ------------------------------------------------------------------ */

void BlockDev_Close(struct BlockDev *bd)
{
    if (!bd) return;
    if (bd->open) CloseDevice((struct IORequest *)&bd->iotd);
    local_delete_port(bd->port);
    FreeVec(bd);
}

/* ------------------------------------------------------------------ */
/* BlockDev_ReadBlock                                                  */
/* ------------------------------------------------------------------ */

BOOL BlockDev_ReadBlock(struct BlockDev *bd, ULONG blocknum, void *buf)
{
    bd->iotd.iotd_Req.io_Command = CMD_READ;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = buf;
    bd->iotd.iotd_Req.io_Offset  = blocknum * bd->block_size;
    bd->iotd.iotd_Req.io_Flags   = 0;
    return DoIO((struct IORequest *)&bd->iotd) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* BlockDev_WriteBlock                                                 */
/* ------------------------------------------------------------------ */

BOOL BlockDev_WriteBlock(struct BlockDev *bd, ULONG blocknum, const void *buf)
{
    bd->iotd.iotd_Req.io_Command = CMD_WRITE;
    bd->iotd.iotd_Req.io_Length  = bd->block_size;
    bd->iotd.iotd_Req.io_Data    = (APTR)buf;
    bd->iotd.iotd_Req.io_Offset  = blocknum * bd->block_size;
    bd->iotd.iotd_Req.io_Flags   = 0;
    if (DoIO((struct IORequest *)&bd->iotd) != 0) return FALSE;

    bd->iotd.iotd_Req.io_Command = CMD_UPDATE;
    bd->iotd.iotd_Req.io_Length  = 0;
    bd->iotd.iotd_Req.io_Data    = NULL;
    bd->iotd.iotd_Req.io_Flags   = 0;
    return DoIO((struct IORequest *)&bd->iotd) == 0 ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* BlockDev_HasMBR                                                     */
/* ------------------------------------------------------------------ */

BOOL BlockDev_HasMBR(struct BlockDev *bd)
{
    UBYTE buf[512];
    if (!BlockDev_ReadBlock(bd, 0, buf)) return FALSE;
    return (buf[510] == 0x55 && buf[511] == 0xAA) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/* FormatDosType / FormatSize                                          */
/* ------------------------------------------------------------------ */

void FormatDosType(ULONG dostype, char *buf)
{
    char a = (char)((dostype >> 24) & 0xFF);
    char b = (char)((dostype >> 16) & 0xFF);
    char c = (char)((dostype >>  8) & 0xFF);
    UBYTE ver = (UBYTE)(dostype & 0xFF);
    if (a >= 32 && b >= 32 && c >= 32)
        sprintf(buf, "%c%c%c\\%u", a, b, c, (unsigned)ver);
    else
        sprintf(buf, "0x%08lX", dostype);
}

void FormatSize(UQUAD bytes, char *buf)
{
    if      (bytes >= (UQUAD)1024*1024*1024) sprintf(buf, "%lu GB", (unsigned long)(bytes / (1024*1024*1024)));
    else if (bytes >= (UQUAD)1024*1024)      sprintf(buf, "%lu MB", (unsigned long)(bytes / (1024*1024)));
    else if (bytes >= (UQUAD)1024)           sprintf(buf, "%lu KB", (unsigned long)(bytes / 1024));
    else                                      sprintf(buf, "%lu B",  (unsigned long)bytes);
}

/* ------------------------------------------------------------------ */
/* RDB_Read                                                            */
/* ------------------------------------------------------------------ */

BOOL RDB_Read(struct BlockDev *bd, struct RDBInfo *rdb)
{
    struct RigidDiskBlock *rdsk;
    struct PartitionBlock *pb;
    UBYTE *buf;
    ULONG  blk, next;

    memset(rdb, 0, sizeof(*rdb));
    rdb->valid = FALSE;

    buf = (UBYTE *)AllocVec(512, MEMF_PUBLIC);
    if (!buf)
        return FALSE;

    /* Scan first RDB_SCAN_LIMIT blocks for RDSK signature */
    for (blk = 0; blk < RDB_SCAN_LIMIT; blk++) {
        if (!BlockDev_ReadBlock(bd, blk, buf))
            continue;
        rdsk = (struct RigidDiskBlock *)buf;
        if (rdsk->rdb_ID != IDNAME_RIGIDDISK)
            continue;

        rdb->valid       = TRUE;
        rdb->block_num   = blk;
        rdb->flags       = rdsk->rdb_Flags;
        rdb->cylinders   = rdsk->rdb_Cylinders;
        rdb->sectors     = rdsk->rdb_Sectors;
        rdb->heads       = rdsk->rdb_Heads;
        rdb->rdb_block_lo= rdsk->rdb_RDBBlocksLo;
        rdb->rdb_block_hi= rdsk->rdb_RDBBlocksHi;
        rdb->lo_cyl      = rdsk->rdb_LoCylinder;
        rdb->hi_cyl      = rdsk->rdb_HiCylinder;
        rdb->part_list   = rdsk->rdb_PartitionList;
        rdb->fshdr_list  = rdsk->rdb_FileSysHeaderList;

        memcpy(rdb->disk_vendor,   rdsk->rdb_DiskVendor,   8);  rdb->disk_vendor[8]   = '\0';
        memcpy(rdb->disk_product,  rdsk->rdb_DiskProduct,  16); rdb->disk_product[16] = '\0';
        memcpy(rdb->disk_revision, rdsk->rdb_DiskRevision,  4); rdb->disk_revision[4] = '\0';
        break;
    }

    if (!rdb->valid) {
        FreeVec(buf);
        return FALSE;
    }

    /* Walk partition linked list */
    next = rdb->part_list;
    while (next != RDB_END_MARK && rdb->num_parts < MAX_PARTITIONS) {
        ULONG *env;
        UBYTE *bstr;
        UBYTE  len;
        struct PartInfo *pi;

        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        pb = (struct PartitionBlock *)buf;
        if (pb->pb_ID != IDNAME_PARTITION)
            break;

        pi = &rdb->parts[rdb->num_parts];
        pi->block_num = next;
        pi->next_part = pb->pb_Next;
        pi->flags     = pb->pb_Flags;

        /* BSTR drive name */
        bstr = pb->pb_DriveName;
        len  = bstr[0];
        if (len >= (UBYTE)sizeof(pi->drive_name))
            len = (UBYTE)(sizeof(pi->drive_name) - 1);
        memcpy(pi->drive_name, bstr + 1, len);
        pi->drive_name[len] = '\0';

        /* DosEnvec array */
        env = pb->pb_Environment;
        pi->low_cyl      = env[DE_LOWCYL];
        pi->high_cyl     = env[DE_UPPERCYL];
        pi->heads        = env[DE_NUMHEADS];
        pi->sectors      = env[DE_BLKSPERTRACK];
        pi->block_size   = env[DE_SIZEBLOCK] * 4;
        pi->dos_type     = env[DE_DOSTYPE];
        pi->boot_pri     = (LONG)env[DE_BOOTPRI];
        pi->max_transfer = env[DE_MAXTRANSFER];
        pi->mask         = env[DE_MASK];
        pi->num_buffer   = env[DE_NUMBUFFERS];
        pi->buf_mem_type = env[DE_BUFMEMTYPE];
        /* DE_BOOTBLOCKS = 19; only present when table size covers it */
        pi->boot_blocks  = (env[DE_TABLESIZE] >= 19) ? env[19] : 2;

        next = pb->pb_Next;
        rdb->num_parts++;
    }

    /* Walk FSHD linked list */
    next = rdb->fshdr_list;
    while (next != RDB_END_MARK && rdb->num_fs < MAX_FILESYSTEMS) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi;
        ULONG lseg_blk;
        ULONG num_lseg;

        if (!BlockDev_ReadBlock(bd, next, buf))
            break;
        fhb = (struct FileSysHeaderBlock *)buf;
        if (fhb->fhb_ID != IDNAME_FSHEADER)
            break;

        fi = &rdb->filesystems[rdb->num_fs];
        fi->block_num    = next;
        fi->next_fshd    = fhb->fhb_Next;
        fi->flags        = fhb->fhb_Flags;
        fi->dos_type     = fhb->fhb_DosType;
        fi->version      = fhb->fhb_Version;
        fi->patch_flags  = fhb->fhb_PatchFlags;
        fi->stack_size   = fhb->fhb_StackSize;
        fi->priority     = fhb->fhb_Priority;
        fi->global_vec   = fhb->fhb_GlobalVec;
        fi->seg_list_blk = (ULONG)fhb->fhb_SegListBlocks;
        fi->code         = NULL;
        fi->code_size    = 0;

        next = fhb->fhb_Next;

        /* Count LSEG blocks to allocate exact buffer */
        num_lseg = 0;
        lseg_blk = fi->seg_list_blk;
        while (lseg_blk != RDB_END_MARK) {
            struct LoadSegBlock *lsb;
            if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) break;
            lsb = (struct LoadSegBlock *)buf;
            if (lsb->lsb_ID != IDNAME_LOADSEG) break;
            num_lseg++;
            lseg_blk = lsb->lsb_Next;
        }

        if (num_lseg > 0) {
            ULONG alloc_sz = num_lseg * 492UL;  /* 123 longs * 4 bytes = 492 bytes/block */
            fi->code = (UBYTE *)AllocVec(alloc_sz, MEMF_PUBLIC | MEMF_CLEAR);
            if (fi->code) {
                ULONG offset = 0;
                lseg_blk = fi->seg_list_blk;
                while (lseg_blk != RDB_END_MARK && offset < alloc_sz) {
                    struct LoadSegBlock *lsb;
                    if (!BlockDev_ReadBlock(bd, lseg_blk, buf)) break;
                    lsb = (struct LoadSegBlock *)buf;
                    if (lsb->lsb_ID != IDNAME_LOADSEG) break;
                    memcpy(fi->code + offset, lsb->lsb_LoadData, 492);
                    offset += 492;
                    lseg_blk = lsb->lsb_Next;
                }
                fi->code_size = alloc_sz;
            }
        }

        rdb->num_fs++;
    }

    FreeVec(buf);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RDB_FreeCode — free all AllocVec'd filesystem code buffers          */
/* ------------------------------------------------------------------ */

void RDB_FreeCode(struct RDBInfo *rdb)
{
    UWORD i;
    if (!rdb) return;
    for (i = 0; i < rdb->num_fs; i++) {
        if (rdb->filesystems[i].code) {
            FreeVec(rdb->filesystems[i].code);
            rdb->filesystems[i].code      = NULL;
            rdb->filesystems[i].code_size = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Block checksum                                                       */
/* Sum of all 128 longwords (including the checksum field) must = 0.   */
/* Set checksum field to 0, compute, store -sum in checksum field.     */
/* ------------------------------------------------------------------ */

static LONG block_checksum(const ULONG *buf, ULONG num_longs)
{
    ULONG sum = 0, i;
    for (i = 0; i < num_longs; i++) sum += buf[i];
    return (LONG)(-sum);
}

/* ------------------------------------------------------------------ */
/* RDB_InitFresh                                                        */
/* ------------------------------------------------------------------ */

void RDB_InitFresh(struct RDBInfo *rdb,
                   ULONG cylinders, ULONG heads, ULONG sectors)
{
    memset(rdb, 0, sizeof(*rdb));
    rdb->valid        = TRUE;
    rdb->cylinders    = cylinders;
    rdb->heads        = heads;
    rdb->sectors      = sectors;
    rdb->block_num    = 0;
    rdb->rdb_block_lo = 0;
    rdb->rdb_block_hi = 15;   /* reserve first 16 blocks (RDB_LOCATION_LIMIT) */
    rdb->lo_cyl       = 1;    /* cylinder 0 holds RDB metadata */
    rdb->hi_cyl       = cylinders - 1;
    rdb->part_list    = RDB_END_MARK;
    rdb->fshdr_list   = RDB_END_MARK;
    rdb->flags        = 0;
    rdb->num_parts    = 0;
    rdb->num_fs       = 0;
}

/* ------------------------------------------------------------------ */
/* write_lseg_chain — write filesystem code as a chain of LSEG blocks */
/* ------------------------------------------------------------------ */

static BOOL write_lseg_chain(struct BlockDev *bd, ULONG *blk_buf,
                              const UBYTE *code, ULONG code_size,
                              ULONG first_blk)
{
    ULONG num_blocks = (code_size + 491UL) / 492UL;
    ULONG i;

    for (i = 0; i < num_blocks; i++) {
        ULONG blk   = first_blk + i;
        ULONG next  = (i + 1 < num_blocks) ? blk + 1 : RDB_END_MARK;
        ULONG off   = i * 492UL;
        ULONG chunk = ((off + 492UL) <= code_size) ? 492UL : (code_size - off);

        memset(blk_buf, 0, 512);
        blk_buf[0] = IDNAME_LOADSEG;  /* lsb_ID          */
        blk_buf[1] = 128;             /* lsb_SummedLongs  */
        blk_buf[2] = 0;               /* lsb_ChkSum       */
        blk_buf[3] = 7;               /* lsb_HostID       */
        blk_buf[4] = next;            /* lsb_Next         */
        memcpy((UBYTE *)blk_buf + 20, code + off, chunk);  /* lsb_LoadData (offset 5 longs = 20 bytes) */
        blk_buf[2] = (ULONG)block_checksum(blk_buf, 128);

        if (!BlockDev_WriteBlock(bd, blk, blk_buf)) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* RDB_Write                                                            */
/*                                                                      */
/* Layout:                                                              */
/*   rdb_block_lo+0          = RigidDiskBlock (RDSK)                  */
/*   rdb_block_lo+1 .. +N    = PartitionBlocks (N = num_parts)        */
/*   +N+1 .. +N+F            = FileSysHeaderBlocks (F = num_fs)       */
/*   +N+F+1 .. end           = LoadSegBlocks (filesystem code)        */
/* ------------------------------------------------------------------ */

BOOL RDB_Write(struct BlockDev *bd, struct RDBInfo *rdb)
{
    struct RigidDiskBlock *rdsk;
    struct PartitionBlock *pb;
    ULONG *buf;
    UWORD  i;
    ULONG  part_blk, fshd_blk, lseg_blk;
    ULONG  lseg_starts[MAX_FILESYSTEMS];
    ULONG  last_used_blk;

    if (!bd || !rdb || !rdb->valid) return FALSE;

    buf = (ULONG *)AllocVec(512, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) return FALSE;

    /* First partition block immediately after the RDB block */
    part_blk = rdb->rdb_block_lo + 1;
    fshd_blk = part_blk + rdb->num_parts;

    /* Pre-calculate where each FS's LSEG chain will start */
    lseg_blk = fshd_blk + rdb->num_fs;
    for (i = 0; i < rdb->num_fs; i++) {
        struct FSInfo *fi = &rdb->filesystems[i];
        if (fi->code && fi->code_size > 0) {
            lseg_starts[i] = lseg_blk;
            lseg_blk += (fi->code_size + 491UL) / 492UL;
        } else {
            lseg_starts[i] = RDB_END_MARK;
        }
    }
    /* lseg_blk now points one past the last used block */
    if (lseg_blk > fshd_blk)
        last_used_blk = lseg_blk - 1;
    else if (rdb->num_parts > 0)
        last_used_blk = part_blk + rdb->num_parts - 1;
    else
        last_used_blk = rdb->rdb_block_lo;

    rdb->part_list  = (rdb->num_parts > 0) ? part_blk : RDB_END_MARK;
    rdb->fshdr_list = (rdb->num_fs   > 0) ? fshd_blk : RDB_END_MARK;

    /* --- Write PartitionBlocks --- */
    for (i = 0; i < rdb->num_parts; i++) {
        struct PartInfo *pi  = &rdb->parts[i];
        ULONG            next = (i + 1 < rdb->num_parts)
                                ? (part_blk + 1) : RDB_END_MARK;
        UBYTE *bstr;
        UBYTE  len;

        pi->block_num = part_blk;
        pi->next_part = next;

        memset(buf, 0, 512);
        pb = (struct PartitionBlock *)buf;

        pb->pb_ID          = IDNAME_PARTITION;
        pb->pb_SummedLongs = 512 / 4;
        pb->pb_ChkSum      = 0;
        pb->pb_HostID      = 7;
        pb->pb_Next        = next;
        pb->pb_Flags       = pi->flags;
        pb->pb_DevFlags    = 0;

        /* BSTR drive name */
        bstr = pb->pb_DriveName;
        len  = (UBYTE)strlen(pi->drive_name);
        if (len > 30) len = 30;
        bstr[0] = len;
        memcpy(bstr + 1, pi->drive_name, len);

        /* DosEnvec — index 19 (DE_BOOTBLOCKS) is the highest index we fill */
        pb->pb_Environment[DE_TABLESIZE]    = 19;
        pb->pb_Environment[DE_SIZEBLOCK]    = 128;   /* 512 bytes / 4 */
        pb->pb_Environment[DE_SECORG]       = 0;
        pb->pb_Environment[DE_NUMHEADS]     =
            pi->heads   > 0 ? pi->heads   : rdb->heads;
        pb->pb_Environment[DE_SECSPERBLK]   = 1;
        pb->pb_Environment[DE_BLKSPERTRACK] =
            pi->sectors > 0 ? pi->sectors : rdb->sectors;
        pb->pb_Environment[DE_RESERVEDBLKS] = 2;
        pb->pb_Environment[DE_PREFAC]       = 0;
        pb->pb_Environment[DE_INTERLEAVE]   = 0;
        pb->pb_Environment[DE_LOWCYL]       = pi->low_cyl;
        pb->pb_Environment[DE_UPPERCYL]     = pi->high_cyl;
        pb->pb_Environment[DE_NUMBUFFERS]   =
            pi->num_buffer   > 0 ? pi->num_buffer   : 30;
        pb->pb_Environment[DE_MEMBUFTYPE]   =
            pi->buf_mem_type > 0 ? pi->buf_mem_type : 1;
        pb->pb_Environment[DE_MAXTRANSFER]  =
            pi->max_transfer > 0 ? pi->max_transfer : 0x7FFFFFFFUL;
        pb->pb_Environment[DE_MASK]         =
            pi->mask > 0         ? pi->mask         : 0xFFFFFFFEUL;
        pb->pb_Environment[DE_BOOTPRI]      = (ULONG)(LONG)pi->boot_pri;
        pb->pb_Environment[DE_DOSTYPE]      = pi->dos_type;
        pb->pb_Environment[17]              = 0;   /* DE_BAUD    — unused */
        pb->pb_Environment[18]              = 0;   /* DE_CONTROL — unused */
        pb->pb_Environment[19]              =      /* DE_BOOTBLOCKS */
            pi->boot_blocks > 0 ? pi->boot_blocks : 2;

        pb->pb_ChkSum = block_checksum(buf, 512 / 4);

        if (!BlockDev_WriteBlock(bd, part_blk, buf)) {
            FreeVec(buf);
            return FALSE;
        }
        part_blk++;
    }

    /* --- Write FileSysHeaderBlocks --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FileSysHeaderBlock *fhb;
        struct FSInfo *fi  = &rdb->filesystems[i];
        ULONG next_fshd = (i + 1 < rdb->num_fs) ? fshd_blk + i + 1 : RDB_END_MARK;

        fi->block_num = fshd_blk + i;

        memset(buf, 0, 512);
        fhb = (struct FileSysHeaderBlock *)buf;

        fhb->fhb_ID           = IDNAME_FSHEADER;
        fhb->fhb_SummedLongs  = 128;
        fhb->fhb_ChkSum       = 0;
        fhb->fhb_HostID       = 7;
        fhb->fhb_Next         = next_fshd;
        fhb->fhb_Flags        = fi->flags;
        fhb->fhb_DosType      = fi->dos_type;
        fhb->fhb_Version      = fi->version;
        fhb->fhb_PatchFlags   = fi->patch_flags  ? fi->patch_flags  : 0x180UL;
        fhb->fhb_Type         = 0;
        fhb->fhb_Task         = 0;
        fhb->fhb_Lock         = 0;
        fhb->fhb_Handler      = 0;
        fhb->fhb_StackSize    = fi->stack_size   ? fi->stack_size   : 0x2000UL;
        fhb->fhb_Priority     = fi->priority;
        fhb->fhb_Startup      = 0;
        fhb->fhb_SegListBlocks = (LONG)lseg_starts[i];
        fhb->fhb_GlobalVec    = fi->global_vec   ? fi->global_vec   : -1L;

        fhb->fhb_ChkSum = (LONG)block_checksum(buf, 128);

        if (!BlockDev_WriteBlock(bd, fshd_blk + i, buf)) {
            FreeVec(buf);
            return FALSE;
        }
    }

    /* --- Write LoadSegBlock chains (filesystem code) --- */
    for (i = 0; i < rdb->num_fs; i++) {
        struct FSInfo *fi = &rdb->filesystems[i];
        if (fi->code && fi->code_size > 0 && lseg_starts[i] != RDB_END_MARK) {
            if (!write_lseg_chain(bd, buf, fi->code, fi->code_size, lseg_starts[i])) {
                FreeVec(buf);
                return FALSE;
            }
        }
    }

    /* --- Write RigidDiskBlock --- */
    memset(buf, 0, 512);
    rdsk = (struct RigidDiskBlock *)buf;

    rdsk->rdb_ID          = IDNAME_RIGIDDISK;
    rdsk->rdb_SummedLongs = 512 / 4;
    rdsk->rdb_ChkSum      = 0;
    rdsk->rdb_HostID      = 7;
    rdsk->rdb_BlockBytes  = 512;
    rdsk->rdb_Flags       = rdb->flags;

    /* Optional block list heads: 0xFFFFFFFF = none */
    rdsk->rdb_BadBlockList      = RDB_END_MARK;
    rdsk->rdb_PartitionList     = rdb->part_list;
    rdsk->rdb_FileSysHeaderList = rdb->fshdr_list;
    rdsk->rdb_DriveInit         = RDB_END_MARK;

    /* Reserved1[6] must be 0xFFFFFFFF per spec */
    {
        UWORD r;
        for (r = 0; r < 6; r++)
            rdsk->rdb_Reserved1[r] = 0xFFFFFFFFUL;
    }

    /* Physical drive characteristics */
    rdsk->rdb_Cylinders    = rdb->cylinders;
    rdsk->rdb_Sectors      = rdb->sectors;
    rdsk->rdb_Heads        = rdb->heads;
    rdsk->rdb_Interleave   = 0;
    rdsk->rdb_Park         = rdb->cylinders;
    rdsk->rdb_WritePreComp = 0;
    rdsk->rdb_ReducedWrite = 0;
    rdsk->rdb_StepRate     = 0;

    /* Logical drive characteristics */
    rdsk->rdb_RDBBlocksLo    = rdb->rdb_block_lo;
    rdsk->rdb_RDBBlocksHi    = last_used_blk;
    rdsk->rdb_LoCylinder     = rdb->lo_cyl;
    rdsk->rdb_HiCylinder     = rdb->hi_cyl;
    rdsk->rdb_CylBlocks      = rdb->heads * rdb->sectors;
    rdsk->rdb_AutoParkSeconds = 0;
    rdsk->rdb_HighRDSKBlock  = last_used_blk;

    /* Drive identification strings (preserve if read earlier) */
    memcpy(rdsk->rdb_DiskVendor,   rdb->disk_vendor,   8);
    memcpy(rdsk->rdb_DiskProduct,  rdb->disk_product,  16);
    memcpy(rdsk->rdb_DiskRevision, rdb->disk_revision, 4);

    rdsk->rdb_ChkSum = block_checksum(buf, 512 / 4);

    if (!BlockDev_WriteBlock(bd, rdb->block_num, buf)) {
        FreeVec(buf);
        return FALSE;
    }

    FreeVec(buf);
    return TRUE;
}
