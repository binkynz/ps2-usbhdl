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

/* Run the create + format + stream sequence for one planned ISO.
 * Returns 0 on success, -1 on any failure. */
static int install_one(const install_plan_t *plan, const char *iso_path)
{
	int exists = partition_exists(plan->partition_name);
	if (exists < 0) {
		scr_printf("  ABORT: cannot enumerate hdd0:\n");
		return -1;
	}
	if (exists > 0) {
		scr_printf("  %s exists; removing for clean reinstall\n",
		           plan->partition_name);
		int rret = partition_remove(plan->partition_name);
		if (rret < 0) {
			scr_printf("  remove failed: %d\n", rret);
			return -1;
		}
	}
	if (execute_install(plan) < 0) return -1;
	return stream_iso_to_partition(plan, iso_path);
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

	static int selected[MAX_ISOS];
	int n_selected = pick_isos(isos, iso_count, selected);
	if (n_selected == 0) {
		scr_printf("  aborted by user\n");
		return;
	}
	scr_printf("  selected %d ISO(s)\n", n_selected);

	/* Compute and print plans for everything we'd install. */
	static install_plan_t plans[MAX_ISOS];
	int i;
	for (i = 0; i < iso_count; i++) {
		if (!selected[i]) continue;
		scr_printf("\n  -- %s --\n", isos[i]);
		compute_install_plan(isos[i], &plans[i]);
		print_install_plan(&plans[i]);
	}

	if (!wet_run_authorized()) {
		scr_printf("\n  (touch mass:/INSTALL_NOW to enable wet run)\n");
		return;
	}

	scr_printf("\n  WET RUN (%d ISO%s) in 10s. POWER OFF NOW to abort.\n",
	           n_selected, n_selected == 1 ? "" : "s");
	delay_ms(10000);

	int ok = 0, failed = 0;
	int batch_idx = 0;
	for (i = 0; i < iso_count; i++) {
		if (!selected[i]) continue;
		batch_idx++;
		scr_printf("\n  === %d/%d: %s ===\n",
		           batch_idx, n_selected, isos[i]);
		if (install_one(&plans[i], isos[i]) == 0) ok++;
		else failed++;
	}

	scr_printf("\n  batch done: %d ok, %d failed\n", ok, failed);
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
