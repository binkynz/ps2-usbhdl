#include <debug.h>
#include <dirent.h>
#include <fcntl.h>
#include <kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdl.h"
#include "iop.h"
#include "iso.h"
#include "ui.h"

/* Run the create + format + stream sequence for one planned ISO.
 * Returns 0 on success, -1 on any failure. */
static int install_one(const install_plan_t *plan, const char *iso_path) {
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
  if (execute_install(plan) < 0)
    return -1;
  return stream_iso_to_partition(plan, iso_path);
}

/* Install flow: enumerate USB ISOs, batch-pick, plan, install
 * after a 10-second countdown if anything was selected. */
static void plan_install_from_usb(void) {
  scr_printf("\n  install mode\n");
  scr_printf("  waiting for USB...\n");

  DIR *d = NULL;
  int tries;
  for (tries = 0; tries < 30; tries++) {
    d = opendir("mass:/");
    if (d)
      break;
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
    if (iso_count >= MAX_ISOS)
      continue;
    if (ends_with_iso(de->d_name)) {
      /* Single-file ISO. */
      snprintf(isos[iso_count], sizeof(isos[0]), "mass:/%s", de->d_name);
      iso_count++;
    } else if (is_iso_split_first_part(de->d_name)) {
      /* Split set — strip the trailing ".001" so the picker shows
       * the logical path. iso_file_open will probe parts itself. */
      char base[260];
      size_t name_len = strlen(de->d_name);
      size_t base_len = name_len - 4;
      if (base_len >= sizeof(base))
        continue;
      memcpy(base, de->d_name, base_len);
      base[base_len] = 0;
      snprintf(isos[iso_count], sizeof(isos[0]), "mass:/%s", base);
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
  uint32_t max_partition_mb = hdd_get_max_partition_size_mb();
  int i;
  for (i = 0; i < iso_count; i++) {
    if (!selected[i])
      continue;
    scr_printf("\n  -- %s --\n", isos[i]);
    compute_install_plan(isos[i], &plans[i], max_partition_mb);
    print_install_plan(&plans[i]);
  }

  /* Clear so the WET RUN prompt is unmissable — without this it
   * lands at the bottom of a screen full of plan output and gets
   * wrapped past row 27 by the debug-screen framebuffer. */
  scr_clear();
  scr_setXY(0, 0);
  scr_printf("\n  WET RUN (%d ISO%s) in 10s. POWER OFF NOW to abort.\n",
             n_selected, n_selected == 1 ? "" : "s");
  delay_ms(10000);

  int ok = 0, failed = 0;
  int batch_idx = 0;
  for (i = 0; i < iso_count; i++) {
    if (!selected[i])
      continue;
    batch_idx++;
    scr_printf("\n  === %d/%d: %s ===\n", batch_idx, n_selected, isos[i]);
    if (install_one(&plans[i], isos[i]) == 0)
      ok++;
    else
      failed++;
  }

  scr_printf("\n  batch done: %d ok, %d failed\n", ok, failed);
}

/* Manage flow: list installed HDL partitions, batch-pick, delete
 * after a 10-second confirmation window. */
static void manage_hdl_partitions(void) {
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

  static char display[MAX_PARTS][280];
  int i;
  for (i = 0; i < n; i++)
    snprintf(display[i], sizeof(display[0]), "%-24.32s %5u MB", names[i],
             (unsigned)sizes[i]);

  static int selected[MAX_PARTS];
  int n_sel = pick_items(display, n, selected);
  if (n_sel == 0) {
    scr_printf("  aborted by user\n");
    return;
  }

  /* Same clear-before-warning rationale as the install flow: with
   * many partitions listed the warning would fall off the screen. */
  scr_clear();
  scr_setXY(0, 0);
  scr_printf("\n  DELETE %d partition%s in 10s. POWER OFF to abort.\n", n_sel,
             n_sel == 1 ? "" : "s");
  delay_ms(10000);

  int ok = 0, failed = 0;
  for (i = 0; i < n; i++) {
    if (!selected[i])
      continue;
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

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  init_scr();
  scr_printf("\n  ps2-usbhdl\n");
  scr_printf("  build: " __DATE__ " " __TIME__ "\n");

  boot_iop_with_modules();
  show_hdd();

  if (!pad_init()) {
    scr_printf("\n  no controller detected.\n");
    scr_printf("  connect one and re-run.\n");
    SleepThread();
    return 0;
  }

  int mode = pick_mode();
  switch (mode) {
  case 0:
    plan_install_from_usb();
    break;
  case 1:
    manage_hdl_partitions();
    break;
  default:
    scr_printf("\n  exit selected\n");
    break;
  }

  scr_printf("\n  done. press any button to exit.\n");
  wait_for_any_button();
  return 0;
}
