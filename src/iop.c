#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <debug.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <libpwroff.h>
#include <sbv_patches.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

#include "iop.h"

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
extern unsigned char sio2man_irx[];       extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[];        extern unsigned int size_padman_irx;

/* ps2hdd-hdl.irx wants -o N -n N (max open files, max mounts) as a
 * null-separated argv blob. Values match HDLGameInstaller's loader. */
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

void boot_iop_with_modules(void)
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
	fails += load_irx("sio2man",     sio2man_irx,     size_sio2man_irx,     0, NULL) < 0;
	fails += load_irx("padman",      padman_irx,      size_padman_irx,      0, NULL) < 0;

	scr_printf("  IOP modules: %s\n",
	           fails == 0 ? "all 13 ok" : "FAILURES above");
}
