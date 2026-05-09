#ifndef PS2USBHDL_UI_H
#define PS2USBHDL_UI_H

#define MAX_ISOS 16

/* Real kernel-backed delay (DelayThread under the hood). */
void delay_ms(int ms);

/* D-pad navigable multi-select picker. Writes 1/0 into selected[]
 * for each ISO; returns the count of selections (0 = aborted with
 * Triangle). Controls: D-pad move, Square toggles current row,
 * Cross confirms (auto-selects current row if nothing toggled
 * yet), Triangle aborts.
 *
 * With exactly one ISO we skip the menu, mark it selected, and
 * return 1. With no pad attached we fall back to selecting only
 * the first ISO. */
int  pick_isos(char isos[MAX_ISOS][280], int count,
               int selected[MAX_ISOS]);

#endif
