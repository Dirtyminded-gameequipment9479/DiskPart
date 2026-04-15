/*
 * cli.c — DiskPart CLI mode.
 *
 * Invoked by main() when the program is run from a shell with arguments.
 * ReadArgs() parses the command line; output goes to the current console
 * (redirectable: DiskPart LISTDEV >ram:devs.txt).
 *
 * Adding a new command:
 *   1. Add a keyword to CLI_TEMPLATE + a matching ARG_ enum value.
 *   2. Write a static cmd_xxx() function.
 *   3. Dispatch it from cli_run().
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "devices.h"
#include "rdb.h"
#include "script.h"
#include "cli.h"

extern struct ExecBase   *SysBase;
extern struct DosLibrary *DOSBase;

/* ------------------------------------------------------------------ */
/* Argument template                                                   */
/* ------------------------------------------------------------------ */

/*
 * To add a new command: append the keyword + type and add an ARG_ enum.
 * Keep ARG_COUNT last; enum order must match template keyword order.
 */
#define CLI_TEMPLATE  "LISTDEV/S,UNITS/S,DEV/K,INIT/K,FORCE/S,SCRIPT/K,DRYRUN/S"

enum {
    ARG_LISTDEV = 0,
    ARG_UNITS,
    ARG_DEV,
    ARG_INIT,
    ARG_FORCE,
    ARG_SCRIPT,
    ARG_DRYRUN,
    ARG_COUNT
};

/* ------------------------------------------------------------------ */
/* Shared statics (too large / too slow to put on stack)              */
/* ------------------------------------------------------------------ */

static char          outbuf[400];
static struct RDBInfo s_rdb;          /* shared across all cmd_* functions */

/* ------------------------------------------------------------------ */
/* Output helper                                                       */
/* ------------------------------------------------------------------ */

static void cli_puts(const char *s)
{
    PutStr((CONST_STRPTR)s);
}

/* ------------------------------------------------------------------ */
/* parse_dev — split "uaehf.device:3" into name + unit               */
/* ------------------------------------------------------------------ */

static BOOL parse_dev(const char *str, char *devname, ULONG *unit)
{
    const char *p = str;
    ULONG len;

    while (*p && *p != ':') p++;
    len = (ULONG)(p - str);
    if (len == 0 || len > 63) return FALSE;

    memcpy(devname, str, len);
    devname[len] = '\0';
    *unit = (*p == ':') ? strtoul(p + 1, NULL, 10) : 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* str_eq_ci — case-insensitive strcmp                                 */
/* ------------------------------------------------------------------ */

static BOOL str_eq_ci(const char *a, const char *b)
{
    for (;;) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return FALSE;
        if (ca == '\0') return TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* ask_yn — interactive Y/N prompt                                     */
/*                                                                     */
/* With force=TRUE: prints the question with "[Y]" and returns TRUE.  */
/* With force=FALSE: prompts and reads one line from stdin.            */
/* Returns TRUE for Y/y, FALSE for anything else (including errors).  */
/* ------------------------------------------------------------------ */

static BOOL ask_yn(const char *question, BOOL force)
{
    char buf[8];
    LONG got;

    if (force) {
        sprintf(outbuf, "%s (Y/N): Y\n", question);
        cli_puts(outbuf);
        return TRUE;
    }

    sprintf(outbuf, "%s (Y/N): ", question);
    cli_puts(outbuf);
    Flush(Output());   /* ensure prompt appears before Read() blocks */

    got = Read(Input(), buf, (LONG)(sizeof(buf) - 1));
    if (got <= 0) return FALSE;
    buf[got] = '\0';
    return (BOOL)(buf[0] == 'Y' || buf[0] == 'y');
}

/* ------------------------------------------------------------------ */
/* print_dev_info — one-line device summary                           */
/* ------------------------------------------------------------------ */

static void print_dev_info(struct BlockDev *bd)
{
    char szbuf[20];
    FormatSize(bd->total_bytes, szbuf);
    if (bd->disk_brand[0])
        sprintf(outbuf, "Device : %s unit %lu  [%s  %s]\n",
                bd->devname, (ULONG)bd->unit, bd->disk_brand, szbuf);
    else
        sprintf(outbuf, "Device : %s unit %lu  [%s]\n",
                bd->devname, (ULONG)bd->unit, szbuf);
    cli_puts(outbuf);
}

/* ------------------------------------------------------------------ */
/* LISTDEV                                                             */
/* ------------------------------------------------------------------ */

static struct DevNameList s_devnames;   /* ~5 KB — static avoids stack pressure */

static LONG cmd_listdev(BOOL probe_units)
{
    UWORD i;

    Devices_Scan(&s_devnames);

    if (s_devnames.count == 0) {
        cli_puts("No block devices found.\n");
        return RETURN_OK;
    }

    sprintf(outbuf, "Block devices: %u\n\n", (unsigned)s_devnames.count);
    cli_puts(outbuf);

    for (i = 0; i < s_devnames.count; i++) {

        if (s_devnames.vers[i] > 0)
            sprintf(outbuf, "%-32s v%u.%u\n",
                    s_devnames.names[i],
                    (unsigned)s_devnames.vers[i],
                    (unsigned)s_devnames.revs[i]);
        else
            sprintf(outbuf, "%s\n", s_devnames.names[i]);
        cli_puts(outbuf);

        if (probe_units) {
            static struct UnitList ul;
            UWORD j;

            sprintf(outbuf, "  Probing units for %s...\n",
                    s_devnames.names[i]);
            cli_puts(outbuf);

            Devices_GetUnitsForName(s_devnames.names[i], &ul, NULL, NULL);

            if (ul.count == 0) {
                cli_puts("  (no units found)\n");
            } else {
                for (j = 0; j < ul.count; j++) {
                    sprintf(outbuf, "  %s\n", ul.entries[j].display);
                    cli_puts(outbuf);
                }
            }
            cli_puts("\n");
        }
    }

    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEW — create a fresh RDB on a blank or overwrite disk         */
/* ------------------------------------------------------------------ */

static LONG cmd_init_new(struct BlockDev *bd, BOOL force)
{
    ULONG cyls, heads, sects;
    char  szbuf[20];
    const char *question;

    print_dev_info(bd);

    /* Check for existing RDB — affects question wording only */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (RDB_Read(bd, &s_rdb) && s_rdb.valid) {
        sprintf(outbuf,
                "WARNING: existing RDB found  "
                "(%lu cylinders, %u partition(s)).\n",
                (ULONG)s_rdb.cylinders, (unsigned)s_rdb.num_parts);
        cli_puts(outbuf);
        RDB_FreeCode(&s_rdb);
        question = "Destroy existing RDB and write a new one?";
    } else {
        question = "Write new RDB?";
    }

    /* Read physical geometry */
    if (!BlockDev_GetGeometry(bd, &cyls, &heads, &sects)) {
        cli_puts("ERROR: Cannot read disk geometry.\n");
        return RETURN_ERROR;
    }

    {
        UQUAD total = (UQUAD)cyls * heads * sects * 512UL;
        FormatSize(total, szbuf);
        sprintf(outbuf, "Geometry : %lu cyls x %lu heads x %lu sectors  (%s)\n",
                cyls, heads, sects, szbuf);
        cli_puts(outbuf);
    }

    if (!ask_yn(question, force))
        return RETURN_OK;

    RDB_InitFresh(&s_rdb, cyls, heads, sects);

    cli_puts("Writing RDB... ");
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts("FAILED.\n");
        return RETURN_ERROR;
    }
    cli_puts("OK.\n");
    cli_puts("RDB written. No partitions yet.\n");
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* INIT NEWGEO — expand RDB cylinder count to match actual disk size  */
/* ------------------------------------------------------------------ */

static LONG cmd_init_newgeo(struct BlockDev *bd, BOOL force)
{
    ULONG new_cyls;
    char  szbuf_old[20], szbuf_new[20];

    print_dev_info(bd);

    /* Require an existing RDB */
    memset(&s_rdb, 0, sizeof(s_rdb));
    if (!RDB_Read(bd, &s_rdb) || !s_rdb.valid) {
        cli_puts("ERROR: No valid RDB found. Use INIT NEW first.\n");
        return RETURN_ERROR;
    }

    if (s_rdb.heads == 0 || s_rdb.sectors == 0) {
        cli_puts("ERROR: RDB has invalid geometry (heads or sectors = 0).\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }

    if (bd->total_bytes == 0) {
        cli_puts("ERROR: Cannot determine actual disk size.\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }

    /* New cylinder count: keep existing H/S, re-derive C from actual sectors */
    new_cyls = (ULONG)((bd->total_bytes / 512UL)
                       / ((UQUAD)s_rdb.heads * s_rdb.sectors));

    {
        UQUAD old_size = (UQUAD)s_rdb.cylinders * s_rdb.heads
                         * s_rdb.sectors * 512UL;
        FormatSize(old_size,       szbuf_old);
        FormatSize(bd->total_bytes, szbuf_new);
    }

    sprintf(outbuf,
            "RDB geometry : %lu cylinders  (%s)\n"
            "Disk reports : %lu cylinders  (%s)\n",
            (ULONG)s_rdb.cylinders, szbuf_old,
            new_cyls,               szbuf_new);
    cli_puts(outbuf);

    if (new_cyls <= s_rdb.cylinders) {
        cli_puts("Disk is not larger than RDB geometry. No update needed.\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    sprintf(outbuf, "Cylinder count will change: %lu -> %lu\n",
            (ULONG)s_rdb.cylinders, new_cyls);
    cli_puts(outbuf);

    if (!ask_yn("Update RDB cylinder count?", force)) {
        RDB_FreeCode(&s_rdb);
        return RETURN_OK;
    }

    s_rdb.cylinders = new_cyls;
    s_rdb.hi_cyl    = new_cyls - 1;
    /* lo_cyl stays the same — partitions are already at their cylinder offsets */

    cli_puts("Writing updated RDB... ");
    if (!RDB_Write(bd, &s_rdb)) {
        cli_puts("FAILED.\n");
        RDB_FreeCode(&s_rdb);
        return RETURN_ERROR;
    }
    cli_puts("OK.\n");

    sprintf(outbuf, "RDB updated to %lu cylinders (%s).\n",
            new_cyls, szbuf_new);
    cli_puts(outbuf);

    RDB_FreeCode(&s_rdb);
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* cmd_init — open device, dispatch NEW / NEWGEO                      */
/* ------------------------------------------------------------------ */

static LONG cmd_init(const char *devname, ULONG unit,
                     const char *mode, BOOL force)
{
    struct BlockDev *bd;
    LONG rc;

    if (!str_eq_ci(mode, "NEW") && !str_eq_ci(mode, "NEWGEO")) {
        sprintf(outbuf,
                "INIT: unknown mode \"%s\". Use NEW or NEWGEO.\n", mode);
        cli_puts(outbuf);
        return RETURN_WARN;
    }

    sprintf(outbuf, "Opening %s unit %lu...\n", devname, unit);
    cli_puts(outbuf);

    bd = BlockDev_Open(devname, unit);
    if (!bd) {
        sprintf(outbuf, "ERROR: Cannot open %s unit %lu.\n", devname, unit);
        cli_puts(outbuf);
        return RETURN_ERROR;
    }

    if (str_eq_ci(mode, "NEW"))
        rc = cmd_init_new(bd, force);
    else
        rc = cmd_init_newgeo(bd, force);

    BlockDev_Close(bd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* cli_run — parse and dispatch                                        */
/* ------------------------------------------------------------------ */

LONG cli_run(void)
{
    LONG args[ARG_COUNT];
    struct RDArgs *rdargs;
    LONG rc = RETURN_OK;

    memset(args, 0, sizeof(args));

    rdargs = ReadArgs((STRPTR)CLI_TEMPLATE, args, NULL);
    if (!rdargs) {
        PrintFault(IoErr(), (STRPTR)"DiskPart");
        return RETURN_ERROR;
    }

    /* No recognised command → empty command line, caller opens GUI */
    if (!args[ARG_LISTDEV] && !args[ARG_INIT] && !args[ARG_SCRIPT]) {
        FreeArgs(rdargs);
        return CLI_NO_ARGS;
    }

    if (args[ARG_LISTDEV])
        rc = cmd_listdev((BOOL)args[ARG_UNITS]);

    if (rc == RETURN_OK && args[ARG_INIT]) {
        if (!args[ARG_DEV]) {
            cli_puts("INIT requires DEV <device>:<unit>  "
                     "(e.g. DEV uaehf.device:3).\n");
            rc = RETURN_WARN;
        } else {
            char  devname[64];
            ULONG unit;
            if (!parse_dev((const char *)args[ARG_DEV], devname, &unit)) {
                cli_puts("ERROR: DEV format must be <device>:<unit> "
                         "(e.g. uaehf.device:3).\n");
                rc = RETURN_WARN;
            } else {
                rc = cmd_init(devname, unit,
                              (const char *)args[ARG_INIT],
                              (BOOL)args[ARG_FORCE]);
            }
        }
    }

    if (rc == RETURN_OK && args[ARG_SCRIPT])
        rc = script_run((const char *)args[ARG_SCRIPT],
                        (BOOL)args[ARG_DRYRUN],
                        (BOOL)args[ARG_FORCE]);

    FreeArgs(rdargs);
    return rc;
}
