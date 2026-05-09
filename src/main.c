#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <kernel.h>
#include <delaythread.h>
#include <debug.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>

/* NEWLIB_PORT_AWARE silences PS2SDK's #error guard that otherwise
 * warns against direct fileXio use under newlib. We need fileXioInit
 * because libhdd assumes the RPC has been started. */
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <libhdd.h>

extern unsigned char iomanX_irx[];        extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];       extern unsigned int size_fileXio_irx;
extern unsigned char poweroff_irx[];      extern unsigned int size_poweroff_irx;
extern unsigned char ps2dev9_irx[];       extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[];       extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_hdl_irx[];    extern unsigned int size_ps2hdd_hdl_irx;
extern unsigned char hdlfs_irx[];         extern unsigned int size_hdlfs_irx;
extern unsigned char usbd_irx[];          extern unsigned int size_usbd_irx;
extern unsigned char bdm_irx[];           extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[];   extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char usbmass_bd_irx[];    extern unsigned int size_usbmass_bd_irx;

/* HDL partition layout constants. Main partitions reserve 4 MB
 * for the HDL header zone (APA + ICON3D + system.cnf + icon + KELF);
 * sub-partitions reserve ~1 MB. The APA driver only accepts six
 * specific size strings ("128M" through "4G") — listed below in
 * APA_BUCKETS — so we round up to one of those rather than to an
 * arbitrary 128 MB grain. The hardware APA cap is 16 GB per
 * partition, but HDLFS records slice size in a 32-bit field and
 * overflows at 4 GB, so 4 GB is our effective ceiling. */
#define HDL_MAIN_RESERVE_MB     4
#define HDL_SUB_RESERVE_MB      1
#define HDL_MAX_PARTITION_MB    4096

struct apa_bucket {
	const char *str;
	uint32_t mb;
};
static const struct apa_bucket APA_BUCKETS[] = {
	{ "128M",  128 },
	{ "256M",  256 },
	{ "512M",  512 },
	{ "1G",   1024 },
	{ "2G",   2048 },
	{ "4G",   4096 },
};
#define APA_BUCKET_COUNT (sizeof(APA_BUCKETS) / sizeof(APA_BUCKETS[0]))

/* Smallest bucket whose size >= needed_mb. Returns the largest bucket
 * if nothing is big enough (caller is expected to spill to subs). */
static int pick_bucket(uint32_t needed_mb)
{
	for (size_t i = 0; i < APA_BUCKET_COUNT; i++)
		if (APA_BUCKETS[i].mb >= needed_mb) return (int)i;
	return APA_BUCKET_COUNT - 1;
}

/* ps2hdd-hdl.irx wants -o N -n N (max open files, max mounts) as a
 * null-separated argv blob with arg_len = sizeof(buffer). The
 * specific values match HDLGameInstaller's own loader. */
static const char ps2hdd_args[] = "-o" "\0" "4" "\0" "-n" "\0" "128";

/* Returns 0 on success, -1 on failure. Silent on success — caller
 * prints a summary instead so module noise doesn't overflow the
 * 27-line debug screen. */
static int load_irx(const char *label, void *data, unsigned int size,
                    int arg_len, const char *args)
{
	int rv = -1;
	int ret = SifExecModuleBuffer(data, size, arg_len, args, &rv);
	if (ret < 0 || rv != 0) {
		scr_printf("  FAIL %s ret=%d rv=%d\n", label, ret, rv);
		return -1;
	}
	return 0;
}

/* Real kernel-backed delay. DelayThread takes microseconds. */
static void delay_ms(int ms)
{
	DelayThread(ms * 1000);
}

static void boot_iop_with_modules(void)
{
	SifInitRpc(0);

	while (!SifIopReset("", 0)) { }
	while (!SifIopSync()) { }
	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();

	/* Required so SifExecModuleBuffer actually starts the modules. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	int fails = 0;
	fails += load_irx("iomanX",      iomanX_irx,      size_iomanX_irx,      0, NULL) < 0;
	fails += load_irx("fileXio",     fileXio_irx,     size_fileXio_irx,     0, NULL) < 0;
	fileXioInit();
	fails += load_irx("poweroff",    poweroff_irx,    size_poweroff_irx,    0, NULL) < 0;
	poweroffInit();
	fails += load_irx("ps2dev9",     ps2dev9_irx,     size_ps2dev9_irx,     0, NULL) < 0;
	fails += load_irx("ps2atad",     ps2atad_irx,     size_ps2atad_irx,     0, NULL) < 0;
	fails += load_irx("ps2hdd-hdl",  ps2hdd_hdl_irx,  size_ps2hdd_hdl_irx,
	                  sizeof(ps2hdd_args), ps2hdd_args) < 0;
	fails += load_irx("hdlfs",       hdlfs_irx,       size_hdlfs_irx,       0, NULL) < 0;
	fails += load_irx("usbd",        usbd_irx,        size_usbd_irx,        0, NULL) < 0;
	fails += load_irx("bdm",         bdm_irx,         size_bdm_irx,         0, NULL) < 0;
	fails += load_irx("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL) < 0;
	fails += load_irx("usbmass_bd",  usbmass_bd_irx,  size_usbmass_bd_irx,  0, NULL) < 0;

	scr_printf("  IOP modules: %s\n",
	           fails == 0 ? "all 11 ok" : "FAILURES above");
}

static void show_hdd(void)
{
	if (hddCheckPresent() != 0 || hddCheckFormatted() != 0) {
		scr_printf("\n  HDD: not present or not APA-formatted\n");
		return;
	}

	t_hddInfo info;
	hddGetInfo(&info);
	scr_printf("\n  HDD: %lu MB free of %lu MB (max-part %lu MB)\n",
	           (unsigned long)info.hddFree,
	           (unsigned long)info.hddSize,
	           (unsigned long)info.hddMaxPartitionSize);

	t_hddFilesystem fs[16];
	int n = hddGetFilesystemList(fs, 16);
	scr_printf("  PFS parts (%d):", n);
	int i;
	for (i = 0; i < n && i < 4; i++)
		scr_printf(" %s%s", fs[i].name, i + 1 < n && i + 1 < 4 ? "," : "");
	if (n > 4) scr_printf(" ...");
	scr_printf("\n");
}

static int ends_with_iso(const char *name)
{
	int n = strlen(name);
	if (n < 5) return 0;
	const char *e = name + n - 4;
	return (e[0] == '.' &&
	        (e[1] == 'i' || e[1] == 'I') &&
	        (e[2] == 's' || e[2] == 'S') &&
	        (e[3] == 'o' || e[3] == 'O'));
}

/* Mirror of HDLGameInstaller's hdlfs/hdlfs.h struct HDLFS_FormatArgs.
 * Passed to fileXioFormat("hdl0:", partname, &args, sizeof(args))
 * after the partition has been created. The hdlfs.irx module reads
 * this and writes the HDL header bytes (game info magic, partition
 * descriptors, system.cnf etc.) into the first 4 MB of the partition.
 * Layout must match exactly — wrong padding here = wrong on-disk
 * header = unbootable game (or worse, a confused APA driver). */
#define HDLFS_GAME_TITLE_LEN  160
#define HDLFS_STARTUP_PTH_LEN 60

struct HDLFS_FormatArgs {
	uint8_t  CompatFlags;
	uint8_t  DiscType;       /* 0x12 = CD, 0x14 = DVD */
	uint8_t  TRType;
	uint8_t  TRMode;
	uint32_t NumSectors;     /* total 2 KB sectors of disc data */
	uint32_t Layer1Start;    /* sector offset of layer 1 for DVD9, else 0 */
	char     GameTitle[HDLFS_GAME_TITLE_LEN];
	char     StartupPath[HDLFS_STARTUP_PTH_LEN];
};

/* Plan the HDL partition layout for an ISO. Pure computation — no
 * disk side-effects, suitable for dry-run display. */
typedef struct {
	int valid;
	char volume_id[33];          /* from ISO9660 PVD */
	char startup_id[16];         /* from SYSTEM.CNF, e.g. "SCES_503.62" */
	uint32_t iso_size_mb;
	uint32_t iso_sectors_2k;     /* PVD blocks × block_size / 2048 */
	char partition_name[33];     /* APA partition name, e.g. PP.HDL.SCES_503.62 */
	uint32_t main_part_size_mb;
	const char *main_size_str;   /* APA bucket label, e.g. "4G" */
	int subs_needed;             /* >0 only for ISOs > ~4 GB */
	uint32_t subs_total_size_mb;
	uint8_t  disc_type;          /* HDLFS DiscType: 0x12 CD / 0x14 DVD */
	uint32_t layer1_start;       /* 0 unless DVD9 */
} install_plan_t;

typedef struct {
	uint32_t lba;
	uint32_t size;
} iso_extent_t;

/* Keep [A-Z0-9_.]; uppercase a-z; drop everything else. Truncate.
 * Dots are allowed because canonical PS2 startup ids contain them
 * (e.g. SCES_503.62) and APA partition names accept dots. */
static void sanitize_for_partname(const char *src, char *dst, int dstsz)
{
	int i = 0, j = 0;
	while (src[i] && j < dstsz - 1) {
		char c = src[i++];
		if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '_' || c == '.')
			dst[j++] = c;
	}
	dst[j] = 0;
}

/* Read sectors from an open ISO. PS2 lseek is 32-bit, but the
 * metadata we need (root dir, SYSTEM.CNF) lives at low LBAs so this
 * is fine here. */
static int read_sector(int fd, uint32_t lba, void *buf)
{
	off_t want = (off_t)lba * 2048;
	if (lseek(fd, want, SEEK_SET) != want) return -1;
	if (read(fd, buf, 2048) != 2048) return -1;
	return 0;
}

/* Walk an ISO9660 directory looking for an entry whose name matches
 * `target` (case-sensitive; ISO9660 names are uppercase by convention).
 * Returns 1 if found and writes the entry's extent to `out`, 0 if
 * not found, -1 on read error. */
static int find_in_dir(int fd, iso_extent_t dir, const char *target,
                       iso_extent_t *out)
{
	int target_len = strlen(target);
	uint32_t bytes_left = dir.size;
	uint32_t lba = dir.lba;
	static unsigned char sector[2048];

	while (bytes_left > 0) {
		if (read_sector(fd, lba, sector) < 0) return -1;

		uint32_t off = 0;
		uint32_t this_chunk = bytes_left < 2048 ? bytes_left : 2048;
		while (off < this_chunk) {
			uint8_t rec_len = sector[off];
			if (rec_len == 0) break; /* zero-pad to end of sector */

			uint32_t extent_lba =
				sector[off + 2] | (sector[off + 3] << 8) |
				(sector[off + 4] << 16) | (sector[off + 5] << 24);
			uint32_t data_len =
				sector[off + 10] | (sector[off + 11] << 8) |
				(sector[off + 12] << 16) | (sector[off + 13] << 24);
			uint8_t name_len = sector[off + 32];
			const char *name = (const char *)&sector[off + 33];

			/* Match `target` exactly OR with ISO9660's ;version
			 * suffix (e.g. SYSTEM.CNF;1). */
			if (name_len >= target_len &&
			    memcmp(name, target, target_len) == 0 &&
			    (name_len == target_len || name[target_len] == ';')) {
				out->lba = extent_lba;
				out->size = data_len;
				return 1;
			}
			off += rec_len;
		}

		if (bytes_left <= 2048) break;
		bytes_left -= 2048;
		lba++;
	}
	return 0;
}

/* Pull the startup id (e.g. "SCES_503.62") from a PS2 ISO's
 * SYSTEM.CNF. The file is text:
 *   BOOT2 = cdrom0:\SCES_503.62;1
 *   VER = 1.00
 *   VMODE = NTSC
 * We extract the bit between "cdrom0:\" and ";". Returns 0 on
 * success, -1 on any failure (file not found, parse failure, etc.). */
static int extract_startup_id(int fd, const unsigned char *pvd,
                              char *out, int out_sz)
{
	/* Root directory record sits at PVD offset 156, 34 bytes. */
	iso_extent_t root;
	root.lba  = pvd[156 + 2] | (pvd[156 + 3] << 8) |
	            (pvd[156 + 4] << 16) | (pvd[156 + 5] << 24);
	root.size = pvd[156 + 10] | (pvd[156 + 11] << 8) |
	            (pvd[156 + 12] << 16) | (pvd[156 + 13] << 24);

	iso_extent_t syscnf;
	if (find_in_dir(fd, root, "SYSTEM.CNF", &syscnf) != 1)
		return -1;

	/* SYSTEM.CNF is tiny — clamp at one sector. */
	if (syscnf.size > 2048) syscnf.size = 2048;
	static char buf[2049];
	if (read_sector(fd, syscnf.lba, buf) < 0) return -1;
	buf[syscnf.size] = 0;

	const char *p = strstr(buf, "BOOT2");
	if (!p) return -1;
	p += 5;
	while (*p == ' ' || *p == '\t' || *p == '=') p++;
	if (strncmp(p, "cdrom0:\\", 8) == 0 ||
	    strncmp(p, "cdrom0:/", 8) == 0)
		p += 8;

	int n = 0;
	while (*p && *p != ';' && *p != '\r' && *p != '\n' &&
	       *p != ' ' && *p != '\t' && n < out_sz - 1)
		out[n++] = *p++;
	out[n] = 0;
	return n > 0 ? 0 : -1;
}

static int compute_install_plan(const char *iso_path, install_plan_t *plan)
{
	memset(plan, 0, sizeof(*plan));

	int fd = open(iso_path, O_RDONLY);
	if (fd < 0) return -1;

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

	/* ISO size in MB from PVD volume size × block size, plus the
	 * 2 KB sector count which is what HDLFS_FormatArgs.NumSectors
	 * wants. PS2 disc data is always 2048-byte sectors so the
	 * sector count = PVD block count when block_size = 2048 (which
	 * it always is for PS2 ISOs). */
	uint32_t blocks = pvd[80] | (pvd[81] << 8) |
	                  (pvd[82] << 16) | (pvd[83] << 24);
	uint32_t bsz = pvd[128] | (pvd[129] << 8);
	plan->iso_size_mb = (uint32_t)((blocks * (uint64_t)bsz) >> 20);
	plan->iso_sectors_2k = (uint32_t)((blocks * (uint64_t)bsz) >> 11);

	/* Disc type and layer break heuristics. CD ISOs are usually
	 * <800 MB; anything bigger is a DVD. DVD9 (>4.7 GB) needs a
	 * layer-break offset, but detecting that from an ISO file alone
	 * isn't trivial — we punt for now; single-layer DVDs work. */
	plan->disc_type = (plan->iso_size_mb < 800) ? 0x12 : 0x14;
	plan->layer1_start = 0;

	/* Best-effort startup id from SYSTEM.CNF. If it fails we still
	 * have a usable plan, just with a less-canonical partition name. */
	(void)extract_startup_id(fd, pvd, plan->startup_id,
	                         sizeof(plan->startup_id));

	close(fd);

	/* Partition name = "PP.HDL." + canonical startup id, falling back
	 * to sanitized volume id. APA name limit is 32; "PP.HDL." is 7,
	 * leaving 25 for the suffix. */
	char san[33];
	const char *suffix = plan->startup_id[0]
	                     ? plan->startup_id
	                     : plan->volume_id;
	sanitize_for_partname(suffix, san, sizeof(san));
	snprintf(plan->partition_name, sizeof(plan->partition_name),
	         "PP.HDL.%.24s", san);

	/* Size the main partition by picking the smallest APA bucket
	 * that fits iso + header reserve. If even the largest bucket
	 * (4 GB) isn't enough, spill the rest into sub-partitions. */
	uint32_t needed_mb = plan->iso_size_mb + HDL_MAIN_RESERVE_MB;
	if (needed_mb <= HDL_MAX_PARTITION_MB) {
		int bk = pick_bucket(needed_mb);
		plan->main_part_size_mb = APA_BUCKETS[bk].mb;
		plan->main_size_str     = APA_BUCKETS[bk].str;
		plan->subs_needed       = 0;
		plan->subs_total_size_mb = 0;
	} else {
		plan->main_part_size_mb  = HDL_MAX_PARTITION_MB;
		plan->main_size_str      = APA_BUCKETS[APA_BUCKET_COUNT - 1].str;
		uint32_t main_data = HDL_MAX_PARTITION_MB - HDL_MAIN_RESERVE_MB;
		uint32_t remaining = plan->iso_size_mb - main_data;
		plan->subs_needed        = 0;
		plan->subs_total_size_mb = 0;
		while (remaining > 0) {
			int bk = pick_bucket(remaining + HDL_SUB_RESERVE_MB);
			uint32_t this_part = APA_BUCKETS[bk].mb;
			uint32_t this_data = this_part - HDL_SUB_RESERVE_MB;
			if (this_data > remaining) this_data = remaining;
			plan->subs_needed++;
			plan->subs_total_size_mb += this_part;
			remaining -= this_data;
		}
	}

	plan->valid = 1;
	return 0;
}

static void print_install_plan(const install_plan_t *plan)
{
	if (!plan->valid) {
		scr_printf("\n  install plan: invalid (PVD parse failed)\n");
		return;
	}
	scr_printf("\n  install plan (DRY RUN):\n");
	scr_printf("    partname:  %s (%u MB)\n",
	           plan->partition_name, (unsigned)plan->main_part_size_mb);
	scr_printf("    iso:       %s / %u MB / %u sectors\n",
	           plan->startup_id[0] ? plan->startup_id : "?",
	           (unsigned)plan->iso_size_mb,
	           (unsigned)plan->iso_sectors_2k);
	scr_printf("    title:     %s\n", plan->volume_id);
	scr_printf("    fmt args:  disc=0x%02x layer1=%u\n",
	           plan->disc_type, (unsigned)plan->layer1_start);
	if (plan->subs_needed > 0)
		scr_printf("    sub parts: %d (%u MB total)\n",
		           plan->subs_needed, (unsigned)plan->subs_total_size_mb);
	scr_printf("  no writes performed.\n");
}

/* Wet-run gate: returns 1 only if mass:/INSTALL_NOW exists. The
 * sentinel-file approach makes accidental writes impossible — user
 * has to deliberately drop the file on the USB stick. */
static int wet_run_authorized(void)
{
	int fd = open("mass:/INSTALL_NOW", O_RDONLY);
	if (fd < 0) return 0;
	close(fd);
	return 1;
}

/* Walk hdd0:'s partition list looking for an exact name match.
 * Returns 1 if found, 0 if not, -1 on enumeration failure. */
static int partition_exists(const char *target)
{
	int dd = fileXioDopen("hdd0:");
	if (dd < 0) return -1;

	iox_dirent_t de;
	int found = 0;
	while (fileXioDread(dd, &de) > 0) {
		if (strcmp(de.name, target) == 0) {
			found = 1;
			break;
		}
	}
	fileXioDclose(dd);
	return found;
}

/* Actually create the APA partition with HDL type, then write the
 * HDL header via fileXioFormat. On format failure the half-created
 * partition is removed so the disk doesn't accumulate garbage.
 *
 * Note: every fileXio* path that names a partition needs the
 * "hdd0:" device prefix. We store partition_name bare (so it
 * matches what fileXioDread returns) and add the prefix on demand
 * when calling iomanX-routed APIs. */
static int execute_install(const install_plan_t *plan)
{
	char hdd_path[64];      /* hdd0:<name> */
	char create_cmd[80];    /* hdd0:<name>,,,SIZE,HDL */
	snprintf(hdd_path,   sizeof(hdd_path),   "hdd0:%s",
	         plan->partition_name);
	snprintf(create_cmd, sizeof(create_cmd), "%s,,,%s,HDL",
	         hdd_path, plan->main_size_str);

	scr_printf("  fileXioOpen: %s\n", create_cmd);
	int fd = fileXioOpen(create_cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		scr_printf("  open failed: %d\n", fd);
		return -1;
	}
	fileXioClose(fd);

	struct HDLFS_FormatArgs args;
	memset(&args, 0, sizeof(args));
	args.DiscType    = plan->disc_type;
	args.NumSectors  = plan->iso_sectors_2k;
	args.Layer1Start = plan->layer1_start;
	strncpy(args.GameTitle,   plan->volume_id,
	        sizeof(args.GameTitle) - 1);
	strncpy(args.StartupPath, plan->startup_id,
	        sizeof(args.StartupPath) - 1);

	scr_printf("  fileXioFormat: %s\n", hdd_path);
	int ret = fileXioFormat("hdl0:", hdd_path,
	                        (const char *)&args, sizeof(args));
	if (ret < 0) {
		scr_printf("  format failed: %d (removing partition)\n", ret);
		fileXioRemove(hdd_path);
		return -1;
	}

	return 0;
}

/* Stream the ISO into the partition's data area via hdl0:.
 *
 * The hdl0: device is hdlfs.irx's view of an HDL-format partition:
 * mount it pointing at hdd0:partname, then open "hdl0:" for I/O —
 * offset 0 is the start of game data (after the 4 MB HDL header
 * zone), and writes flow through hdlfs into the right offsets in
 * main + sub partitions automatically. This is the same pattern
 * HDLGameInstaller's MountOpenGame uses for its install path. */
static int stream_iso_to_partition(const install_plan_t *plan,
                                   const char *iso_path)
{
	char hdd_path[64];
	snprintf(hdd_path, sizeof(hdd_path), "hdd0:%s", plan->partition_name);

	scr_printf("\n  mounting hdl0:...\n");
	int ret = fileXioMount("hdl0:", hdd_path, FIO_MT_RDWR);
	if (ret < 0) {
		scr_printf("  mount failed: %d\n", ret);
		return -1;
	}

	int hdl_fd = fileXioOpen("hdl0:", O_RDWR);
	if (hdl_fd < 0) {
		scr_printf("  hdl0: open failed: %d\n", hdl_fd);
		fileXioUmount("hdl0:");
		return -1;
	}

	int iso_fd = open(iso_path, O_RDONLY);
	if (iso_fd < 0) {
		scr_printf("  iso open failed: %d\n", iso_fd);
		fileXioClose(hdl_fd);
		fileXioUmount("hdl0:");
		return -1;
	}

	/* 1 MB chunks: a sweet spot between USB-1.1 throughput
	 * (≈1 MB/s) and not blowing too much of the 32 MB EE RAM. */
	static uint8_t buf[1024 * 1024] __attribute__((aligned(64)));

	uint64_t total = (uint64_t)plan->iso_sectors_2k * 2048;
	unsigned total_mb = (unsigned)(total >> 20);
	uint64_t written = 0;
	int last_pct = -1;
	time_t start_t = time(NULL);

	scr_printf("  streaming %u MB (USB 1.1 ≈ 1 MB/s, expect ~%u min)\n",
	           total_mb, (total_mb + 59) / 60);
	int progress_y = scr_getY();

	while (written < total) {
		uint32_t want = (total - written > sizeof(buf))
		                ? (uint32_t)sizeof(buf)
		                : (uint32_t)(total - written);
		int got = read(iso_fd, buf, want);
		if (got <= 0) {
			scr_printf("\n  read err at %u MB: %d\n",
			           (unsigned)(written >> 20), got);
			break;
		}
		int wrote = fileXioWrite(hdl_fd, buf, got);
		if (wrote != got) {
			scr_printf("\n  write err at %u MB: %d/%d\n",
			           (unsigned)(written >> 20), wrote, got);
			break;
		}
		written += wrote;

		int pct = (int)((written * 100) / total);
		if (pct != last_pct) {
			time_t elapsed = time(NULL) - start_t;
			unsigned eta = 0;
			if (elapsed > 0 && written > 0) {
				uint64_t remaining = total - written;
				eta = (unsigned)((remaining * (uint64_t)elapsed)
				                 / written);
			}
			scr_clearline(progress_y);
			scr_setXY(0, progress_y);
			scr_printf("  %u/%u MB (%d%%)  elapsed %u:%02u  ETA %u:%02u",
			           (unsigned)(written >> 20), total_mb, pct,
			           (unsigned)(elapsed / 60),
			           (unsigned)(elapsed % 60),
			           eta / 60, eta % 60);
			last_pct = pct;
		}
	}

	close(iso_fd);
	fileXioClose(hdl_fd);
	fileXioUmount("hdl0:");

	scr_setXY(0, progress_y + 1);
	if (written == total) {
		time_t elapsed = time(NULL) - start_t;
		scr_printf("  install complete in %u:%02u — boot via OPL\n",
		           (unsigned)(elapsed / 60),
		           (unsigned)(elapsed % 60));
		return 0;
	}
	scr_printf("  install incomplete: %u/%u MB written\n",
	           (unsigned)(written >> 20), total_mb);
	return -1;
}

static void maybe_install(const install_plan_t *plan, const char *iso_path)
{
	if (!plan->valid) return;

	if (!wet_run_authorized()) {
		scr_printf("  (touch mass:/INSTALL_NOW to enable wet run)\n");
		return;
	}

	int exists = partition_exists(plan->partition_name);
	if (exists < 0) {
		scr_printf("  ABORT: cannot enumerate hdd0:\n");
		return;
	}

	if (exists > 0) {
		char hdd_path[64];
		snprintf(hdd_path, sizeof(hdd_path), "hdd0:%s",
		         plan->partition_name);
		scr_printf("\n  %s exists; removing for clean reinstall\n",
		           plan->partition_name);
		int rret = fileXioRemove(hdd_path);
		if (rret < 0) {
			scr_printf("  remove failed: %d, aborting\n", rret);
			return;
		}
	}

	scr_printf("\n  WET RUN in 10s. POWER OFF NOW to abort.\n");
	delay_ms(10000);
	if (execute_install(plan) < 0) return;
	stream_iso_to_partition(plan, iso_path);
}

static void plan_install_from_usb(void)
{
	scr_printf("\n  waiting for USB...\n");

	DIR *d = NULL;
	int tries;
	for (tries = 0; tries < 30; tries++) {
		d = opendir("mass:/");
		if (d) break;
		delay_ms(100);
	}
	if (!d) {
		scr_printf("  no usb mount after 3s\n");
		return;
	}

	char first_iso[280] = {0};
	struct dirent *de;
	int total = 0;
	while ((de = readdir(d)) != NULL) {
		total++;
		if (!first_iso[0] && ends_with_iso(de->d_name))
			snprintf(first_iso, sizeof(first_iso),
			         "mass:/%s", de->d_name);
	}
	closedir(d);
	scr_printf("  USB: %d entries\n", total);

	if (!first_iso[0]) {
		scr_printf("  no .iso file found\n");
		return;
	}

	scr_printf("  ISO: %s\n", first_iso);

	install_plan_t plan;
	compute_install_plan(first_iso, &plan);
	print_install_plan(&plan);
	maybe_install(&plan, first_iso);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	init_scr();
	scr_printf("\n  ps2-usbhdl\n");
	scr_printf("  build: " __DATE__ " " __TIME__ "\n");

	boot_iop_with_modules();
	show_hdd();
	plan_install_from_usb();

	scr_printf("\n  done. power-cycle to return.\n");
	SleepThread();
	return 0;
}
