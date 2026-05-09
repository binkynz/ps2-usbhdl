#include <debug.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iso.h"

/* HDL partition layout constants. Main partitions reserve 4 MB
 * for the HDL header zone; sub-partitions reserve ~1 MB. The APA
 * driver only accepts six specific size strings ("128M" through
 * "4G") so we round up to one of those rather than to an arbitrary
 * 128 MB grain. The hardware APA cap is 16 GB per partition, but
 * HDLFS records slice size in a 32-bit field and overflows at
 * 4 GB, so 4 GB is our effective ceiling. */
#define HDL_MAIN_RESERVE_MB 4
#define HDL_SUB_RESERVE_MB 1
#define HDL_MAX_PARTITION_MB 4096

struct apa_bucket {
  const char *str;
  uint32_t mb;
};
static const struct apa_bucket APA_BUCKETS[] = {
    {"128M", 128}, {"256M", 256}, {"512M", 512},
    {"1G", 1024},  {"2G", 2048},  {"4G", 4096},
};
#define APA_BUCKET_COUNT (sizeof(APA_BUCKETS) / sizeof(APA_BUCKETS[0]))

/* Smallest bucket whose size >= needed_mb. Returns the largest
 * bucket if nothing is big enough (caller spills to subs). */
static int pick_bucket(uint32_t needed_mb) {
  for (size_t i = 0; i < APA_BUCKET_COUNT; i++)
    if (APA_BUCKETS[i].mb >= needed_mb)
      return (int)i;
  return APA_BUCKET_COUNT - 1;
}

typedef struct {
  uint32_t lba;
  uint32_t size;
} iso_extent_t;

int ends_with_iso(const char *name) {
  int n = strlen(name);
  if (n < 5)
    return 0;
  const char *e = name + n - 4;
  return (e[0] == '.' && (e[1] == 'i' || e[1] == 'I') &&
          (e[2] == 's' || e[2] == 'S') && (e[3] == 'o' || e[3] == 'O'));
}

/* Keep [A-Z0-9_.]; uppercase a-z; drop everything else. Truncate.
 * Dots are allowed because canonical PS2 startup ids contain them
 * (e.g. SCES_503.62) and APA partition names accept dots. */
static void sanitize_for_partname(const char *src, char *dst, int dstsz) {
  int i = 0, j = 0;
  while (src[i] && j < dstsz - 1) {
    char c = src[i++];
    if (c >= 'a' && c <= 'z')
      c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '.')
      dst[j++] = c;
  }
  dst[j] = 0;
}

/* Read sectors from an open ISO. PS2 lseek is 32-bit, but the
 * metadata we need (root dir, SYSTEM.CNF) lives at low LBAs so
 * this is fine here. */
static int read_sector(int fd, uint32_t lba, void *buf) {
  off_t want = (off_t)lba * 2048;
  if (lseek(fd, want, SEEK_SET) != want)
    return -1;
  if (read(fd, buf, 2048) != 2048)
    return -1;
  return 0;
}

/* Walk an ISO9660 directory looking for an entry whose name matches
 * `target` (case-sensitive; ISO9660 names are uppercase by
 * convention). Returns 1 if found and writes the entry's extent to
 * `out`, 0 if not found, -1 on read error. */
static int find_in_dir(int fd, iso_extent_t dir, const char *target,
                       iso_extent_t *out) {
  int target_len = strlen(target);
  uint32_t bytes_left = dir.size;
  uint32_t lba = dir.lba;
  static unsigned char sector[2048];

  while (bytes_left > 0) {
    if (read_sector(fd, lba, sector) < 0)
      return -1;

    uint32_t off = 0;
    uint32_t this_chunk = bytes_left < 2048 ? bytes_left : 2048;
    while (off < this_chunk) {
      uint8_t rec_len = sector[off];
      if (rec_len == 0)
        break; /* zero-pad to end of sector */

      uint32_t extent_lba = sector[off + 2] | (sector[off + 3] << 8) |
                            (sector[off + 4] << 16) | (sector[off + 5] << 24);
      uint32_t data_len = sector[off + 10] | (sector[off + 11] << 8) |
                          (sector[off + 12] << 16) | (sector[off + 13] << 24);
      uint8_t name_len = sector[off + 32];
      const char *name = (const char *)&sector[off + 33];

      if (name_len >= target_len && memcmp(name, target, target_len) == 0 &&
          (name_len == target_len || name[target_len] == ';')) {
        out->lba = extent_lba;
        out->size = data_len;
        return 1;
      }
      off += rec_len;
    }

    if (bytes_left <= 2048)
      break;
    bytes_left -= 2048;
    lba++;
  }
  return 0;
}

/* Pull the startup id (e.g. "SCES_503.62") from a PS2 ISO's
 * SYSTEM.CNF. Returns 0 on success, -1 on any failure. */
static int extract_startup_id(int fd, const unsigned char *pvd, char *out,
                              int out_sz) {
  iso_extent_t root;
  root.lba = pvd[156 + 2] | (pvd[156 + 3] << 8) | (pvd[156 + 4] << 16) |
             (pvd[156 + 5] << 24);
  root.size = pvd[156 + 10] | (pvd[156 + 11] << 8) | (pvd[156 + 12] << 16) |
              (pvd[156 + 13] << 24);

  iso_extent_t syscnf;
  if (find_in_dir(fd, root, "SYSTEM.CNF", &syscnf) != 1)
    return -1;

  if (syscnf.size > 2048)
    syscnf.size = 2048;
  static char buf[2049];
  if (read_sector(fd, syscnf.lba, buf) < 0)
    return -1;
  buf[syscnf.size] = 0;

  const char *p = strstr(buf, "BOOT2");
  if (!p)
    return -1;
  p += 5;
  while (*p == ' ' || *p == '\t' || *p == '=')
    p++;
  if (strncmp(p, "cdrom0:\\", 8) == 0 || strncmp(p, "cdrom0:/", 8) == 0)
    p += 8;

  int n = 0;
  while (*p && *p != ';' && *p != '\r' && *p != '\n' && *p != ' ' &&
         *p != '\t' && n < out_sz - 1)
    out[n++] = *p++;
  out[n] = 0;
  return n > 0 ? 0 : -1;
}

int compute_install_plan(const char *iso_path, install_plan_t *plan) {
  memset(plan, 0, sizeof(*plan));

  int fd = open(iso_path, O_RDONLY);
  if (fd < 0)
    return -1;

  static unsigned char pvd[2048];
  if (read_sector(fd, 16, pvd) < 0) {
    close(fd);
    return -1;
  }

  if (pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
    close(fd);
    return -1;
  }

  /* Volume identifier (32 bytes at PVD offset 40), trim trailing spaces. */
  memcpy(plan->volume_id, &pvd[40], 32);
  plan->volume_id[32] = 0;
  int j;
  for (j = 31; j >= 0 && plan->volume_id[j] == ' '; j--)
    plan->volume_id[j] = 0;

  /* ISO size in MB and 2 KB sector count from PVD volume size *
   * block size. PS2 discs always use 2048-byte sectors. */
  uint32_t blocks =
      pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) | (pvd[83] << 24);
  uint32_t bsz = pvd[128] | (pvd[129] << 8);
  plan->iso_size_mb = (uint32_t)((blocks * (uint64_t)bsz) >> 20);
  plan->iso_sectors_2k = (uint32_t)((blocks * (uint64_t)bsz) >> 11);

  /* Disc type. CD ISOs are usually <800 MB; anything bigger is
   * a DVD. */
  plan->disc_type = (plan->iso_size_mb < 800) ? 0x12 : 0x14;

  /* Layer-1 start. Single-layer discs use 0. DVD9 needs the
   * sector offset of layer 1, which can't be cleanly recovered
   * from a bare ISO file. Accept a sidecar override at
   * "<iso-path>.layer_break" containing a decimal sector
   * count. */
  plan->layer1_start = 0;
  {
    char sidecar[320];
    snprintf(sidecar, sizeof(sidecar), "%s.layer_break", iso_path);
    int sc = open(sidecar, O_RDONLY);
    if (sc >= 0) {
      char buf[32] = {0};
      int n = read(sc, buf, sizeof(buf) - 1);
      close(sc);
      if (n > 0) {
        buf[n] = 0;
        plan->layer1_start = (uint32_t)atoi(buf);
      }
    }
  }

  /* Best-effort startup id from SYSTEM.CNF. */
  (void)extract_startup_id(fd, pvd, plan->startup_id, sizeof(plan->startup_id));

  close(fd);

  /* Partition name = "PP.HDL." + canonical startup id, falling
   * back to sanitized volume id. */
  char san[33];
  const char *suffix = plan->startup_id[0] ? plan->startup_id : plan->volume_id;
  sanitize_for_partname(suffix, san, sizeof(san));
  snprintf(plan->partition_name, sizeof(plan->partition_name), "PP.HDL.%.24s",
           san);

  /* Size the main partition by picking the smallest APA bucket
   * that fits iso + header reserve. If even the largest bucket
   * (4 GB) isn't enough, spill the rest into sub-partitions. */
  uint32_t needed_mb = plan->iso_size_mb + HDL_MAIN_RESERVE_MB;
  if (needed_mb <= HDL_MAX_PARTITION_MB) {
    int bk = pick_bucket(needed_mb);
    plan->main_part_size_mb = APA_BUCKETS[bk].mb;
    plan->main_size_str = APA_BUCKETS[bk].str;
    plan->subs_needed = 0;
    plan->subs_total_size_mb = 0;
  } else {
    plan->main_part_size_mb = HDL_MAX_PARTITION_MB;
    plan->main_size_str = APA_BUCKETS[APA_BUCKET_COUNT - 1].str;
    uint32_t main_data = HDL_MAX_PARTITION_MB - HDL_MAIN_RESERVE_MB;
    uint32_t remaining = plan->iso_size_mb - main_data;
    plan->subs_needed = 0;
    plan->subs_total_size_mb = 0;
    while (remaining > 0 && plan->subs_needed < MAX_SUBS) {
      int bk = pick_bucket(remaining + HDL_SUB_RESERVE_MB);
      uint32_t this_part = APA_BUCKETS[bk].mb;
      uint32_t this_data = this_part - HDL_SUB_RESERVE_MB;
      if (this_data > remaining)
        this_data = remaining;
      plan->sub_size_strs[plan->subs_needed] = APA_BUCKETS[bk].str;
      plan->subs_needed++;
      plan->subs_total_size_mb += this_part;
      remaining -= this_data;
    }
  }

  plan->valid = 1;
  return 0;
}

void print_install_plan(const install_plan_t *plan) {
  if (!plan->valid) {
    scr_printf("\n  install plan: invalid (PVD parse failed)\n");
    return;
  }
  scr_printf("\n  install plan (DRY RUN):\n");
  scr_printf("    partname:  %s (%u MB)\n", plan->partition_name,
             (unsigned)plan->main_part_size_mb);
  scr_printf("    iso:       %s / %u MB / %u sectors\n",
             plan->startup_id[0] ? plan->startup_id : "?",
             (unsigned)plan->iso_size_mb, (unsigned)plan->iso_sectors_2k);
  scr_printf("    title:     %s\n", plan->volume_id);
  scr_printf("    fmt args:  disc=0x%02x layer1=%u\n", plan->disc_type,
             (unsigned)plan->layer1_start);
  if (plan->iso_size_mb > 4500 && plan->layer1_start == 0)
    scr_printf("    WARNING: DVD9-sized; no .layer_break sidecar\n");
  if (plan->subs_needed > 0) {
    scr_printf("    sub parts: %d (%u MB total)\n", plan->subs_needed,
               (unsigned)plan->subs_total_size_mb);
    int i;
    for (i = 0; i < plan->subs_needed; i++)
      scr_printf("      [%d] %s\n", i + 1, plan->sub_size_strs[i]);
  }
  scr_printf("  no writes performed.\n");
}
