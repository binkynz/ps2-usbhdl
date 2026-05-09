#ifndef PS2USBHDL_UI_H
#define PS2USBHDL_UI_H

#define MAX_ISOS 16  /* upper bound for the install-flow ISO list */
#define MAX_PARTS 24 /* upper bound for the manage-flow partition list */

/* Real kernel-backed delay (DelayThread under the hood). */
void delay_ms(int ms);

/* Bring up port 0 / slot 0 once and wait for the pad to reach
 * STABLE. Returns 1 if a controller responds within timeout, 0
 * otherwise. Idempotent. The picker functions call this on entry
 * but main() should call it once at startup so a missing pad is
 * detected before any user-visible flow runs. */
int pad_init(void);

/* Top-level mode picker. Returns 0 = install, 1 = manage, -1 =
 * exit. Pure button-press dispatch; no cursor / multi-select. */
int pick_mode(void);

/* D-pad navigable multi-select picker, generic over any array of
 * strings. Writes 1/0 into selected[] for each row; returns the
 * count of selections (0 = aborted with Triangle). Controls:
 * D-pad move, Square toggles current row, Cross confirms (auto-
 * selects current row if nothing toggled), Triangle aborts.
 *
 * With exactly one item we skip the menu, mark it selected, and
 * return 1. With no pad attached we fall back to selecting only
 * the first item. */
int pick_items(char items[][280], int count, int selected[]);

/* Block until the user presses any button. Falls back to
 * SleepThread() if the pad isn't available. Used as the final
 * "done — press any button to exit" prompt so the EE/IOP don't
 * idle forever holding the framebuffer + HDD modules awake. */
void wait_for_any_button(void);

#endif
