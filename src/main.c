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

/* ps2hdd-hdl.irx wants -o N -n N (max open files, max mounts) as a
 * null-separated argv blob with arg_len = sizeof(buffer). The
 * specific values match HDLGameInstaller's own loader so we stay on
 * the path it tests. */
static const char ps2hdd_args[] = "-o" "\0" "4" "\0" "-n" "\0" "128";

static int load_irx(const char *label, void *data, unsigned int size,
                    int arg_len, const char *args)
{
	int rv = -1;
	int ret = SifExecModuleBuffer(data, size, arg_len, args, &rv);
	scr_printf("  load %-12s ret=%d rv=%d\n", label, ret, rv);
	return ret;
}

/* Spin for ~ms milliseconds. The USB stack needs a moment after the
 * IRX modules are loaded for device enumeration to complete; without
 * this opendir("mass:/") returns ENOENT. */
static void busy_delay_ms(int ms)
{
	/* PS2 EE runs at ~295 MHz; this loop overestimates which is fine. */
	volatile int i;
	int reps = ms * 20000;
	for (i = 0; i < reps; i++) { /* nop */ }
}

static void boot_iop_with_modules(void)
{
	SifInitRpc(0);

	/* Reset the IOP for a clean module set. */
	while (!SifIopReset("", 0)) { }
	while (!SifIopSync()) { }
	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();

	/* Required so SifExecModuleBuffer actually starts the modules. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	scr_printf("\n  loading IOP modules:\n");
	load_irx("iomanX",      iomanX_irx,      size_iomanX_irx,      0, NULL);
	load_irx("fileXio",     fileXio_irx,     size_fileXio_irx,     0, NULL);
	fileXioInit();
	load_irx("poweroff",    poweroff_irx,    size_poweroff_irx,    0, NULL);
	poweroffInit();
	load_irx("ps2dev9",     ps2dev9_irx,     size_ps2dev9_irx,     0, NULL);
	load_irx("ps2atad",     ps2atad_irx,     size_ps2atad_irx,     0, NULL);
	load_irx("ps2hdd-hdl",  ps2hdd_hdl_irx,  size_ps2hdd_hdl_irx,
	         sizeof(ps2hdd_args), ps2hdd_args);
	load_irx("hdlfs",       hdlfs_irx,       size_hdlfs_irx,       0, NULL);
	load_irx("usbd",        usbd_irx,        size_usbd_irx,        0, NULL);
	load_irx("bdm",         bdm_irx,         size_bdm_irx,         0, NULL);
	load_irx("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL);
	load_irx("usbmass_bd",  usbmass_bd_irx,  size_usbmass_bd_irx,  0, NULL);
}

static void show_partition_table(void)
{
	int present = hddCheckPresent();
	int formatted = hddCheckFormatted();
	scr_printf("\n  HDD checks:\n");
	scr_printf("    present:   %d %s\n", present,
	           present == 0 ? "(ok)" : "(error)");
	scr_printf("    formatted: %d %s\n", formatted,
	           formatted == 0 ? "(ok)" : "(error)");

	if (present != 0 || formatted != 0)
		return;

	t_hddInfo info;
	hddGetInfo(&info);
	scr_printf("\n  HDD info:\n");
	scr_printf("    total: %lu MB\n", (unsigned long)info.hddSize);
	scr_printf("    free:  %lu MB\n", (unsigned long)info.hddFree);
	scr_printf("    max partition: %lu MB\n",
	           (unsigned long)info.hddMaxPartitionSize);

	t_hddFilesystem fs[64];
	int n = hddGetFilesystemList(fs, 64);
	scr_printf("\n  PFS partitions: %d\n", n);
	int i;
	for (i = 0; i < n && i < 10; i++) {
		scr_printf("    [%d] %-16s %lu MB\n",
		           i, fs[i].name, (unsigned long)fs[i].size);
	}
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

static void parse_iso_pvd(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		scr_printf("    open failed: %d\n", fd);
		return;
	}

	/* ISO9660 Primary Volume Descriptor lives at sector 16. */
	if (lseek(fd, 16 * 2048, SEEK_SET) != 16 * 2048) {
		scr_printf("    seek to PVD failed\n");
		close(fd);
		return;
	}

	static unsigned char pvd[2048];
	if (read(fd, pvd, 2048) != 2048) {
		scr_printf("    read PVD failed\n");
		close(fd);
		return;
	}
	close(fd);

	/* Type byte 0x01, signature "CD001". */
	if (pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
		scr_printf("    not a valid ISO9660 PVD (type=%02x)\n", pvd[0]);
		return;
	}

	/* Volume identifier: 32 bytes at offset 40, space-padded. */
	char vol_id[33];
	memcpy(vol_id, &pvd[40], 32);
	vol_id[32] = 0;
	int j;
	for (j = 31; j >= 0 && vol_id[j] == ' '; j--)
		vol_id[j] = 0;

	/* Volume space size: 32-bit little-endian at offset 80
	 * (the PVD also has a big-endian copy at 84 but we trust LE). */
	uint32_t vol_blocks = pvd[80] | (pvd[81] << 8) |
	                      (pvd[82] << 16) | (pvd[83] << 24);
	/* Logical block size: 16-bit little-endian at offset 128. */
	uint32_t block_size = pvd[128] | (pvd[129] << 8);

	scr_printf("    volume id:  %s\n", vol_id);
	scr_printf("    block size: %u\n", (unsigned)block_size);
	scr_printf("    blocks:     %u\n", (unsigned)vol_blocks);
	scr_printf("    iso size:   %u MB\n",
	           (unsigned)((vol_blocks * (uint64_t)block_size) >> 20));
}

static void show_usb_iso(void)
{
	scr_printf("\n  waiting for USB...\n");

	/* Poll opendir until the USB stack enumerates our stick or we
	 * time out. usbmass_bd needs a moment after load to bind. */
	DIR *d = NULL;
	int tries;
	for (tries = 0; tries < 30; tries++) {
		d = opendir("mass:/");
		if (d) break;
		busy_delay_ms(100);
	}
	if (!d) {
		scr_printf("    no usb mount after 3s\n");
		return;
	}

	scr_printf("\n  USB contents (mass:/):\n");
	char first_iso[280] = {0};
	struct dirent *de;
	int count = 0;
	while ((de = readdir(d)) != NULL) {
		if (count < 12)
			scr_printf("    %s\n", de->d_name);
		else if (count == 12)
			scr_printf("    ... (more)\n");
		if (!first_iso[0] && ends_with_iso(de->d_name))
			snprintf(first_iso, sizeof(first_iso),
			         "mass:/%s", de->d_name);
		count++;
	}
	closedir(d);

	if (!first_iso[0]) {
		scr_printf("\n  no .iso file found at mass:/\n");
		return;
	}

	scr_printf("\n  reading PVD from %s\n", first_iso);
	parse_iso_pvd(first_iso);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	init_scr();
	scr_printf("\n  ps2-usbhdl: HDD + USB ISO reader\n");
	scr_printf("  read-only - no writes will occur\n");
	scr_printf("  build: " __DATE__ " " __TIME__ "\n");

	boot_iop_with_modules();
	show_partition_table();
	show_usb_iso();

	scr_printf("\n  done. power-cycle to return.\n");
	SleepThread();
	return 0;
}
