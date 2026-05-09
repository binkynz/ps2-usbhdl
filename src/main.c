#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <kernel.h>
#include <debug.h>

#include "iop.h"
#include "iso.h"
#include "hdl.h"
#include "ui.h"

/* Wet-run gate: returns 1 only if mass:/INSTALL_NOW exists. The
 * sentinel-file approach makes accidental writes impossible —
 * user has to deliberately drop the file on the USB stick. */
static int wet_run_authorized(void)
{
	int fd = open("mass:/INSTALL_NOW", O_RDONLY);
	if (fd < 0) return 0;
	close(fd);
	return 1;
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

	if (exists > 0)
		scr_printf("\n  %s exists; will remove + reinstall.\n",
		           plan->partition_name);

	scr_printf("  WET RUN in 10s. POWER OFF NOW to abort.\n");
	delay_ms(10000);

	/* Past the abort window — destructive ops can run. */
	if (exists > 0) {
		int rret = partition_remove(plan->partition_name);
		if (rret < 0) {
			scr_printf("  remove failed: %d, aborting\n", rret);
			return;
		}
	}
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

	static char isos[MAX_ISOS][280];
	int iso_count = 0;
	int total = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		total++;
		if (iso_count < MAX_ISOS && ends_with_iso(de->d_name)) {
			snprintf(isos[iso_count], sizeof(isos[0]),
			         "mass:/%s", de->d_name);
			iso_count++;
		}
	}
	closedir(d);
	scr_printf("  USB: %d entries (%d .iso)\n", total, iso_count);

	if (iso_count == 0) {
		scr_printf("  no .iso file found\n");
		return;
	}

	int sel = pick_iso(isos, iso_count);
	if (sel < 0) {
		scr_printf("  aborted by user\n");
		return;
	}
	scr_printf("  ISO: %s\n", isos[sel]);

	install_plan_t plan;
	compute_install_plan(isos[sel], &plan);
	print_install_plan(&plan);
	maybe_install(&plan, isos[sel]);
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
