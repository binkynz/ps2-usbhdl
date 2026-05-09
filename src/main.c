#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <kernel.h>
#include <debug.h>

#include "iop.h"
#include "iso.h"
#include "hdl.h"
#include "ui.h"

/* Sentinel-file gate. Returns 1 only if the named file exists at
 * mass:/<name>. Used to require a deliberate per-action opt-in
 * before any destructive op runs. */
static int sentinel_present(const char *name)
{
	char path[64];
	snprintf(path, sizeof(path), "mass:/%s", name);
	int fd = open(path, O_RDONLY);
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

/* Install flow: enumerate USB ISOs, batch-pick, plan, optionally
 * install if INSTALL_NOW is present. */
static void plan_install_from_usb(void)
{
	scr_printf("\n  install mode\n");
	scr_printf("  waiting for USB...\n");

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
	int n_selected = pick_items(isos, iso_count, selected);
	if (n_selected == 0) {
		scr_printf("  aborted by user\n");
		return;
	}
	scr_printf("  selected %d ISO(s)\n", n_selected);

	static install_plan_t plans[MAX_ISOS];
	int i;
	for (i = 0; i < iso_count; i++) {
		if (!selected[i]) continue;
		scr_printf("\n  -- %s --\n", isos[i]);
		compute_install_plan(isos[i], &plans[i]);
		print_install_plan(&plans[i]);
	}

	if (!sentinel_present("INSTALL_NOW")) {
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

/* Manage flow: list installed HDL partitions, batch-pick, delete
 * after a 10-second confirmation window. */
static void manage_hdl_partitions(void)
{
	scr_printf("\n  manage mode\n");

	static char names[MAX_PARTS][33];
	static uint32_t sizes[MAX_PARTS];
	int n = list_hdl_partitions(names, sizes, MAX_PARTS);
	if (n < 0) {
		scr_printf("  ABORT: cannot enumerate hdd0:\n");
		return;
	}
	if (n == 0) {
		scr_printf("  no HDL partitions on hdd0:\n");
		return;
	}
	scr_printf("  %d HDL partition%s:\n", n, n == 1 ? "" : "s");

	/* Build display strings ("PP.HDL.SCUS_971.99 — 4096 MB") for
	 * the picker. */
	static char display[MAX_PARTS][280];
	int i;
	for (i = 0; i < n; i++)
		snprintf(display[i], sizeof(display[0]),
		         "%-24.32s %5u MB",
		         names[i], (unsigned)sizes[i]);

	static int selected[MAX_PARTS];
	int n_sel = pick_items(display, n, selected);
	if (n_sel == 0) {
		scr_printf("  aborted by user\n");
		return;
	}

	scr_printf("\n  DELETE %d partition%s in 10s. POWER OFF to abort.\n",
	           n_sel, n_sel == 1 ? "" : "s");
	delay_ms(10000);

	int ok = 0, failed = 0;
	for (i = 0; i < n; i++) {
		if (!selected[i]) continue;
		scr_printf("  removing %s...\n", names[i]);
		int rret = partition_remove(names[i]);
		if (rret == 0) {
			ok++;
		} else {
			scr_printf("    failed: %d\n", rret);
			failed++;
		}
	}

	scr_printf("\n  delete done: %d ok, %d failed\n", ok, failed);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	init_scr();
	scr_printf("\n  ps2-usbhdl\n");
	scr_printf("  build: " __DATE__ " " __TIME__ "\n");

	boot_iop_with_modules();
	show_hdd();

	/* Mode dispatch via sentinel files on the USB stick. Manage
	 * takes precedence so an accidental MANAGE_NOW + INSTALL_NOW
	 * combo doesn't silently install instead of asking. */
	if (sentinel_present("MANAGE_NOW"))
		manage_hdl_partitions();
	else
		plan_install_from_usb();

	scr_printf("\n  done. power-cycle to return.\n");
	SleepThread();
	return 0;
}
