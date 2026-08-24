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
 * driver accepts specific size strings ("128M" through "32G") so
 * we round up to one of those rather than to an arbitrary 128 MB
 * grain. APA limits a newly-created partition to approximately
 * 1/32 of the HDD capacity; HDL_MAX_PARTITION_MB provides an
 * additional enforced ceiling because HDLFS records slice size in a
 * 32-bit field and overflows at the effective HDL limit. */
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
    {"8G", 8192},  {"16G", 16384},  {"32G", 32768},
};
#define APA_BUCKET_COUNT (sizeof(APA_BUCKETS) / sizeof(APA_BUCKETS[0]))

/* Smallest bucket whose size >= needed_mb and does not exceed
 * max_partition_mb. Returns the largest allowed bucket if nothing
 * is big enough. */
static int pick_bucket(uint32_t needed_mb, uint32_t max_partition_mb) {
  int largest = -1;

  for (size_t i = 0; i < APA_BUCKET_COUNT; i++) {
    if (APA_BUCKETS[i].mb > max_partition_mb)
      break;

    largest = (int)i;

    if (APA_BUCKETS[i].mb >= needed_mb)
      return (int)i;
  }

  return largest;
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

int is_iso_split_first_part(const char *name) {
  int n = strlen(name);
  if (n < 9)
    return 0;
  if (memcmp(name + n - 4, ".001", 4) != 0)
    return 0;
  return (name[n - 8] == '.') && (name[n - 7] == 'i' || name[n - 7] == 'I') &&
         (name[n - 6] == 's' || name[n - 6] == 'S') &&
         (name[n - 5] == 'o' || name[n - 5] == 'O');
}

/* ===========================================================
 * iso_file_t: single-file or split-file logical reader.
 * =========================================================== */

static int iso_file_open_part(iso_file_t *f, int part_idx) {
  char path[300];
  if (f->single_mode)
    snprintf(path, sizeof(path), "%s", f->base);
  else
    snprintf(path, sizeof(path), "%s.%03d", f->base, part_idx + 1);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  f->fd = fd;
  f->cur_part = part_idx;
  f->cur_part_pos = 0;
  return 0;
}

static uint64_t iso_file_part_size(int fd) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  off_t end = lseek(fd, 0, SEEK_END);
  lseek(fd, cur, SEEK_SET);
  if (end == (off_t)-1)
    return 0xFFFFFFFFULL;
  /* PS2SDK off_t is 32-bit signed: a 2-4 GB FAT32 file size fits
   * in unsigned 32-bit but lseek hands it back as a negative
   * signed value, which sign-extends into garbage when widened to
   * uint64_t. Reinterpret through uint32_t to get the true size. */
  return (uint64_t)(uint32_t)end;
}

int iso_file_open(iso_file_t *f, const char *path) {
  memset(f, 0, sizeof(*f));
  f->fd = -1;
  snprintf(f->base, sizeof(f->base), "%s", path);

  /* Single-file fast path: try the path verbatim. */
  int fd = open(path, O_RDONLY);
  if (fd >= 0) {
    f->single_mode = 1;
    f->parts = 1;
    f->part_sizes[0] = iso_file_part_size(fd);
    f->total = f->part_sizes[0];
    f->fd = fd;
    f->cur_part = 0;
    f->cur_part_pos = 0;
    return 0;
  }

  /* Split-file mode: probe <path>.001, .002, ... in order. */
  f->single_mode = 0;
  f->parts = 0;
  f->total = 0;
  while (f->parts < ISO_FILE_MAX_PARTS) {
    char part_path[300];
    snprintf(part_path, sizeof(part_path), "%s.%03d", f->base, f->parts + 1);
    int p = open(part_path, O_RDONLY);
    if (p < 0)
      break;
    f->part_sizes[f->parts] = iso_file_part_size(p);
    f->total += f->part_sizes[f->parts];
    close(p);
    f->parts++;
  }

  if (f->parts == 0)
    return -1;

  return iso_file_open_part(f, 0);
}

int iso_file_seek(iso_file_t *f, uint64_t offset) {
  if (offset > f->total)
    return -1;

  /* Find the part that contains `offset`. */
  uint64_t accum = 0;
  int target_part = 0;
  int i;
  for (i = 0; i < f->parts; i++) {
    if (offset < accum + f->part_sizes[i]) {
      target_part = i;
      break;
    }
    accum += f->part_sizes[i];
  }
  uint64_t local = offset - accum;

  /* Reopen the right part if we're not on it already. */
  if (f->fd < 0 || f->cur_part != target_part) {
    if (f->fd >= 0)
      close(f->fd);
    f->fd = -1;
    if (iso_file_open_part(f, target_part) < 0)
      return -1;
  }

  if (lseek(f->fd, (off_t)local, SEEK_SET) != (off_t)local)
    return -1;
  f->cur_part_pos = local;
  return 0;
}

int iso_file_read(iso_file_t *f, void *buf, uint32_t want) {
  uint8_t *p = (uint8_t *)buf;
  uint32_t total = 0;

  while (want > 0) {
    if (f->fd < 0) {
      if (f->cur_part >= f->parts)
        break;
      if (iso_file_open_part(f, f->cur_part) < 0)
        break;
    }

    int got = read(f->fd, p, want);
    if (got < 0)
      break;
    if (got == 0) {
      /* EOF on this part — roll over to the next, if any. We use
       * read==0 rather than comparing cur_part_pos to part_sizes
       * because PS2SDK off_t is 32-bit signed and lseek(SEEK_END)
       * misreports any FAT32 file >= 2 GB; EOF-on-read is the
       * source of truth at the part boundary. */
      close(f->fd);
      f->fd = -1;
      if (f->cur_part + 1 >= f->parts)
        break;
      f->cur_part++;
      f->cur_part_pos = 0;
      continue;
    }
    p += got;
    total += got;
    want -= got;
    f->cur_part_pos += got;
  }
  return (int)total;
}

void iso_file_close(iso_file_t *f) {
  if (f->fd >= 0)
    close(f->fd);
  f->fd = -1;
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

/* Read one sector through the iso_file_t layer (handles split
 * boundaries even though the metadata we touch — PVD, root dir,
 * SYSTEM.CNF — always lives within the first part). */
static int read_sector(iso_file_t *f, uint32_t lba, void *buf) {
  if (iso_file_seek(f, (uint64_t)lba * 2048) < 0)
    return -1;
  if (iso_file_read(f, buf, 2048) != 2048)
    return -1;
  return 0;
}

/* Walk an ISO9660 directory looking for an entry whose name matches
 * `target` (case-sensitive; ISO9660 names are uppercase by
 * convention). Returns 1 if found and writes the entry's extent to
 * `out`, 0 if not found, -1 on read error. */
static int find_in_dir(iso_file_t *f, iso_extent_t dir, const char *target,
                       iso_extent_t *out) {
  int target_len = strlen(target);
  uint32_t bytes_left = dir.size;
  uint32_t lba = dir.lba;
  static unsigned char sector[2048];

  while (bytes_left > 0) {
    if (read_sector(f, lba, sector) < 0)
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
static int extract_startup_id(iso_file_t *f, const unsigned char *pvd,
                              char *out, int out_sz) {
  iso_extent_t root;
  root.lba = pvd[156 + 2] | (pvd[156 + 3] << 8) | (pvd[156 + 4] << 16) |
             (pvd[156 + 5] << 24);
  root.size = pvd[156 + 10] | (pvd[156 + 11] << 8) | (pvd[156 + 12] << 16) |
              (pvd[156 + 13] << 24);

  iso_extent_t syscnf;
  if (find_in_dir(f, root, "SYSTEM.CNF", &syscnf) != 1)
    return -1;

  if (syscnf.size > 2048)
    syscnf.size = 2048;
  static char buf[2049];
  if (read_sector(f, syscnf.lba, buf) < 0)
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

int compute_install_plan(const char *iso_path, install_plan_t *plan, uint32_t hdd_max_partition_mb) {
  memset(plan, 0, sizeof(*plan));

  uint32_t max_partition_mb = hdd_max_partition_mb;

  if (max_partition_mb > HDL_MAX_PARTITION_MB)
      max_partition_mb = HDL_MAX_PARTITION_MB;

  if (max_partition_mb < APA_BUCKETS[0].mb)
    return -1;

  iso_file_t f;
  if (iso_file_open(&f, iso_path) < 0)
    return -1;

  static unsigned char pvd[2048];
  if (read_sector(&f, 16, pvd) < 0) {
    iso_file_close(&f);
    return -1;
  }

  if (pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
    iso_file_close(&f);
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
  (void)extract_startup_id(&f, pvd, plan->startup_id, sizeof(plan->startup_id));

  iso_file_close(&f);

  /* Partition name = "PP.HDL." + canonical startup id, falling
   * back to sanitized volume id. */
  char san[33];
  const char *suffix = plan->startup_id[0] ? plan->startup_id : plan->volume_id;
  sanitize_for_partname(suffix, san, sizeof(san));
  snprintf(plan->partition_name, sizeof(plan->partition_name), "PP.HDL.%.24s",
           san);

  /* Size the main partition by picking the smallest APA bucket
   * that fits iso + header reserve. If the maximum allowed bucket
   * isn't enough, spill the rest into sub-partitions. */
  uint32_t needed_mb = plan->iso_size_mb + HDL_MAIN_RESERVE_MB;
  if (needed_mb <= max_partition_mb) {
    int bk = pick_bucket(needed_mb, max_partition_mb);
    plan->main_part_size_mb = APA_BUCKETS[bk].mb;
    plan->main_size_str = APA_BUCKETS[bk].str;
    plan->subs_needed = 0;
    plan->subs_total_size_mb = 0;
  } else {
    int main_bk = pick_bucket(max_partition_mb, max_partition_mb);
    plan->main_part_size_mb = APA_BUCKETS[main_bk].mb;
    plan->main_size_str = APA_BUCKETS[main_bk].str;
    uint32_t main_data = plan->main_part_size_mb - HDL_MAIN_RESERVE_MB;
    uint32_t remaining = plan->iso_size_mb - main_data;
    plan->subs_needed = 0;
    plan->subs_total_size_mb = 0;
    while (remaining > 0 && plan->subs_needed < MAX_SUBS) {
      int bk = pick_bucket(remaining + HDL_SUB_RESERVE_MB, max_partition_mb);
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
