#ifndef PS2USBHDL_HDL_H
#define PS2USBHDL_HDL_H

#include "iso.h"

/* Print HDD presence + size summary + PFS partition names. */
void show_hdd(void);

/* 1 if an APA partition with this exact bare name exists, 0 if
 * not, -1 on enumeration failure. Compared against the bare names
 * fileXioDread returns (no "hdd0:" prefix). */
int  partition_exists(const char *target);

/* Remove an APA partition by bare name. Caller passes the same
 * unprefixed name used by partition_exists; this function adds the
 * "hdd0:" prefix internally. Returns 0 on success, negative errno. */
int  partition_remove(const char *target);

/* Create the APA partition + chain any planned sub-partitions +
 * write the HDL header zone via fileXioFormat. On format failure
 * the half-created partition is removed so the disk doesn't
 * accumulate garbage. */
int  execute_install(const install_plan_t *plan);

/* Mount hdl0: against the named partition and stream the ISO into
 * the partition's data area in 1 MB chunks. Renders a 2-line
 * progress bar with throughput / elapsed / ETA, refreshed each
 * percent and at least once per wall-second. */
int  stream_iso_to_partition(const install_plan_t *plan,
                             const char *iso_path);

#endif
