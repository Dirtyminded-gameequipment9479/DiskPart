/*
 * devices.h — Block device enumeration for DiskPart.
 */

#ifndef DEVICES_H
#define DEVICES_H

#include <exec/types.h>

#define MAX_DEV_NAMES     32
#define MAX_KNOWN_DEVICES 64

/* ---- Level-1 list: unique exec device driver names ---- */

struct DevNameList {
    UWORD count;
    char  names[MAX_DEV_NAMES][64];
};

/* ---- Level-2 list: units found for one driver, with disk info ---- */

struct UnitEntry {
    ULONG unit;
    char  display[128];   /* e.g. "Unit 0   WDC WD5000AA   500 MB" */
};

struct UnitList {
    UWORD count;
    struct UnitEntry entries[MAX_KNOWN_DEVICES];
};

/*
 * Devices_Scan — populate nl with all discoverable block device driver names.
 * Phase 1: walk AmigaDOS DosList for mounted devices.
 * Phase 2: walk exec DeviceList for all drivers currently loaded in RAM.
 * No I/O is performed — this is a pure memory walk and completes instantly.
 */
void Devices_Scan(struct DevNameList *nl);

/* Probe progress callback — called before and after each unit open attempt.
 * phase PROBE_START : about to probe this unit  (info = NULL)
 * phase PROBE_FOUND : unit responded            (info = display string)
 * phase PROBE_EMPTY : unit not present          (info = NULL)
 */
#define PROBE_START  0
#define PROBE_FOUND  1
#define PROBE_EMPTY  2

typedef void (*UnitProbeCallback)(void *ud, ULONG unit, UWORD phase,
                                  const char *info);

/*
 * Probe all units for devname and fill ul with the responding ones.
 * For each unit, tries SCSI Inquiry, RDB, then TD_GETGEOMETRY for disk info.
 * cb / cb_data may be NULL.
 */
void Devices_GetUnitsForName(const char *devname, struct UnitList *ul,
                              UnitProbeCallback cb, void *cb_data);

#endif /* DEVICES_H */
