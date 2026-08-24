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

/* Returns 1 if the name matches "*.iso.001" (case-insensitive on
 * the .iso part). The 001 indicates the first part of a split
 * ISO; subsequent parts (.002, .003, ...) are siblings and aren't
 * directly opened by the user. */
int is_iso_split_first_part(const char *name);

int compute_install_plan(const char *iso_path, install_plan_t *plan, uint32_t hdd_max_partition_mb);
void print_install_plan(const install_plan_t *plan);

/* Logical-file abstraction over a single .iso or a split set
 * (.iso.001, .iso.002, ...). All offsets/sizes are in bytes
 * spanning the whole logical file; the implementation handles
 * crossing per-part boundaries.
 *
 * A single .iso has parts == 1 with that one file. A split set
 * is detected by trying <path>.001 if <path> doesn't exist. */
#define ISO_FILE_MAX_PARTS 16

typedef struct {
  int single_mode;
  char base[280]; /* path the user picked, without any .NNN suffix */
  int parts;
  uint64_t part_sizes[ISO_FILE_MAX_PARTS];
  uint64_t total;

  int fd;                /* currently open part fd, -1 if none */
  int cur_part;          /* index into part_sizes */
  uint64_t cur_part_pos; /* byte position within cur_part */
} iso_file_t;

int iso_file_open(iso_file_t *f, const char *path);
int iso_file_seek(iso_file_t *f, uint64_t offset); /* SEEK_SET semantics */
int iso_file_read(iso_file_t *f, void *buf, uint32_t want);
void iso_file_close(iso_file_t *f);

#endif
