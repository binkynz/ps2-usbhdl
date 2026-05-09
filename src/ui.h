#ifndef PS2USBHDL_UI_H
#define PS2USBHDL_UI_H

#define MAX_ISOS 16   /* upper bound for the install-flow ISO list */
#define MAX_PARTS 24  /* upper bound for the manage-flow partition list */

/* Real kernel-backed delay (DelayThread under the hood). */
void delay_ms(int ms);

/* D-pad navigable multi-select picker, generic over any array of
 * strings. Writes 1/0 into selected[] for each row; returns the
 * count of selections (0 = aborted with Triangle). Controls:
 * D-pad move, Square toggles current row, Cross confirms (auto-
 * selects current row if nothing toggled), Triangle aborts.
 *
 * With exactly one item we skip the menu, mark it selected, and
 * return 1. With no pad attached we fall back to selecting only
 * the first item. */
int  pick_items(char items[][280], int count, int selected[]);

#endif
