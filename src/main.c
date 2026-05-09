#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <kernel.h>
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

/* HDL partition layout constants — derived from hdl-dump source.
 * The main partition reserves 0x2000 sectors (4 MB) at the start
 * for the HDL header zone (APA + ICON3D + system.cnf + icon + KELF).
 * Sub-partitions reserve 0x800 sectors (1 MB). APA partitions are
 * allocated in 128 MB grains and capped at 16 GB each. */
#define HDL_MAIN_RESERVE_MB     4
#define HDL_SUB_RESERVE_MB      1
#define APA_GRAIN_MB            128
#define APA_MAX_PARTITION_MB    16384

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

/* Spin for ~ms milliseconds. The USB stack needs a moment after the
 * IRX modules are loaded for device enumeration to complete. */
static void busy_delay_ms(int ms)
{
	volatile int i;
	int reps = ms * 20000;
	for (i = 0; i < reps; i++) { /* nop */ }
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

/* Plan the HDL partition layout for an ISO. Pure computation — no
 * disk side-effects, suitable for dry-run display. */
typedef struct {
	int valid;
	char volume_id[33];          /* from ISO9660 PVD */
	char startup_id[16];         /* from SYSTEM.CNF, e.g. "SCES_503.62" */
	uint32_t iso_size_mb;
	char partition_name[33];     /* APA partition name, e.g. PP.HDL.SCES_503.62 */
	uint32_t main_part_size_mb;
	int subs_needed;             /* >0 only for ISOs > ~16 GB */
	uint32_t subs_total_size_mb;
} install_plan_t;

typedef struct {
	uint32_t lba;
	uint32_t size;
} iso_extent_t;

static uint32_t round_up_to(uint32_t v, uint32_t grain)
{
	return ((v + grain - 1) / grain) * grain;
}

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

	/* ISO size in MB from PVD volume size × block size. */
	uint32_t blocks = pvd[80] | (pvd[81] << 8) |
	                  (pvd[82] << 16) | (pvd[83] << 24);
	uint32_t bsz = pvd[128] | (pvd[129] << 8);
	plan->iso_size_mb = (uint32_t)((blocks * (uint64_t)bsz) >> 20);

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

	/* Size the main partition. If iso + 4 MB header fits in 16 GB,
	 * one main partition is enough (rounded up to 128 MB grain). */
	uint32_t needed_mb = plan->iso_size_mb + HDL_MAIN_RESERVE_MB;
	if (needed_mb <= APA_MAX_PARTITION_MB) {
		plan->main_part_size_mb = round_up_to(needed_mb, APA_GRAIN_MB);
		plan->subs_needed = 0;
		plan->subs_total_size_mb = 0;
	} else {
		/* Larger games need sub-partitions. Each sub stores
		 * (sub_size - 1 MB) of data, capped at 16 GB. */
		plan->main_part_size_mb = APA_MAX_PARTITION_MB;
		uint32_t main_data = APA_MAX_PARTITION_MB - HDL_MAIN_RESERVE_MB;
		uint32_t remaining = plan->iso_size_mb - main_data;
		while (remaining > 0) {
			uint32_t this_data;
			uint32_t this_part;
			if (remaining + HDL_SUB_RESERVE_MB > APA_MAX_PARTITION_MB) {
				this_part = APA_MAX_PARTITION_MB;
				this_data = APA_MAX_PARTITION_MB - HDL_SUB_RESERVE_MB;
			} else {
				this_part = round_up_to(remaining + HDL_SUB_RESERVE_MB,
				                        APA_GRAIN_MB);
				this_data = remaining;
			}
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
	scr_printf("    volume:    %s (%u MB)\n",
	           plan->volume_id, (unsigned)plan->iso_size_mb);
	scr_printf("    startup:   %s\n",
	           plan->startup_id[0] ? plan->startup_id : "(SYSTEM.CNF parse failed)");
	scr_printf("    partname:  %s\n", plan->partition_name);
	scr_printf("    main part: %u MB\n", (unsigned)plan->main_part_size_mb);
	if (plan->subs_needed > 0)
		scr_printf("    sub parts: %d (%u MB total)\n",
		           plan->subs_needed, (unsigned)plan->subs_total_size_mb);
	scr_printf("  no writes performed.\n");
}

static void plan_install_from_usb(void)
{
	scr_printf("\n  waiting for USB...\n");

	DIR *d = NULL;
	int tries;
	for (tries = 0; tries < 30; tries++) {
		d = opendir("mass:/");
		if (d) break;
		busy_delay_ms(100);
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
