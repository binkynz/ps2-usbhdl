#ifndef PS2USBHDL_ISO_H
#define PS2USBHDL_ISO_H

#include <stdint.h>

/* Each game gets one main APA partition plus up to MAX_SUBS
 * extension partitions for ISOs that don't fit in 4 GB (the
 * per-partition cap imposed by HDLFS's 32-bit slice size). PS2
 * discs top out around 8.5 GB (DVD9), which fits in main + 2 subs
 * at the 4 GB bucket; 8 is a comfortable cap. */
#define MAX_SUBS 8

/* Plan the HDL partition layout for an ISO. Pure computation — no
 * disk side-effects, suitable for dry-run display. */
typedef struct {
  int valid;
  char volume_id[33];  /* from ISO9660 PVD */
  char startup_id[16]; /* from SYSTEM.CNF, e.g. "SCES_503.62" */
  uint32_t iso_size_mb;
  uint32_t iso_sectors_2k; /* PVD blocks × block_size / 2048 */
  char partition_name[33]; /* APA partition name, e.g. PP.HDL.SCES_503.62 */
  uint32_t main_part_size_mb;
  const char *main_size_str; /* APA bucket label, e.g. "4G" */
  int subs_needed;           /* >0 only for ISOs > ~4 GB */
  uint32_t subs_total_size_mb;
  const char *sub_size_strs[MAX_SUBS]; /* bucket label per sub */
  uint8_t disc_type;                   /* HDLFS DiscType: 0x12 CD / 0x14 DVD */
  uint32_t layer1_start;               /* 0 unless DVD9 */
} install_plan_t;

int ends_with_iso(const char *name);
int compute_install_plan(const char *iso_path, install_plan_t *plan);
void print_install_plan(const install_plan_t *plan);

#endif
