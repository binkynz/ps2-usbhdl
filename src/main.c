#include <stdio.h>
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

extern unsigned char iomanX_irx[];   extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];  extern unsigned int size_fileXio_irx;
extern unsigned char poweroff_irx[]; extern unsigned int size_poweroff_irx;
extern unsigned char ps2dev9_irx[];  extern unsigned int size_ps2dev9_irx;
extern unsigned char ps2atad_irx[];  extern unsigned int size_ps2atad_irx;
extern unsigned char ps2hdd_irx[];   extern unsigned int size_ps2hdd_irx;

/* ps2hdd.irx wants -o N -n N (max open files, max mounts) as a
 * null-separated argv blob with arg_len = sizeof(buffer). */
static const char ps2hdd_args[] = "-o" "\0" "20" "\0" "-n" "\0" "20";

static int load_irx(const char *label, void *data, unsigned int size,
                    int arg_len, const char *args)
{
	int rv = -1;
	int ret = SifExecModuleBuffer(data, size, arg_len, args, &rv);
	scr_printf("  load %-9s ret=%d rv=%d\n", label, ret, rv);
	return ret;
}

static void boot_iop_with_modules(void)
{
	SifInitRpc(0);

	/* Reset the IOP for a clean module set. The kernel may have
	 * pre-loaded modules we don't want; resetting avoids surprises. */
	while (!SifIopReset("", 0)) { }
	while (!SifIopSync()) { }
	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();

	/* The default rom0:LOADFILE RPC doesn't support
	 * LoadModuleBuffer; without this patch our SifExecModuleBuffer
	 * calls would write bytes but never actually start the module,
	 * which makes downstream Init() calls hang forever waiting for
	 * RPC servers that were never registered. */
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	scr_printf("\n  loading IOP modules:\n");
	load_irx("iomanX",   iomanX_irx,   size_iomanX_irx,   0, NULL);
	load_irx("fileXio",  fileXio_irx,  size_fileXio_irx,  0, NULL);
	scr_printf("    fileXioInit()...\n");
	fileXioInit();
	scr_printf("    fileXioInit ok\n");
	load_irx("poweroff", poweroff_irx, size_poweroff_irx, 0, NULL);
	scr_printf("    poweroffInit()...\n");
	poweroffInit();
	scr_printf("    poweroffInit ok\n");
	load_irx("ps2dev9",  ps2dev9_irx,  size_ps2dev9_irx,  0, NULL);
	load_irx("ps2atad",  ps2atad_irx,  size_ps2atad_irx,  0, NULL);
	load_irx("ps2hdd",   ps2hdd_irx,   size_ps2hdd_irx,
	         sizeof(ps2hdd_args), ps2hdd_args);
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

	if (present != 0 || formatted != 0) {
		scr_printf("\n  cannot enumerate partitions.\n");
		return;
	}

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
	for (i = 0; i < n && i < 25; i++) {
		scr_printf("    [%d] %-16s %lu MB\n",
		           i, fs[i].name, (unsigned long)fs[i].size);
	}
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	init_scr();
	scr_printf("\n  ps2-usbhdl: HDD partition reader\n");
	scr_printf("  read-only - no writes will occur\n");
	scr_printf("  build: " __DATE__ " " __TIME__ "\n");

	boot_iop_with_modules();
	show_partition_table();

	scr_printf("\n  done. power-cycle to return.\n");
	SleepThread();
	return 0;
}
