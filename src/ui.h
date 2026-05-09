#ifndef PS2USBHDL_UI_H
#define PS2USBHDL_UI_H

#define MAX_ISOS 16

/* Real kernel-backed delay (DelayThread under the hood). */
void delay_ms(int ms);

/* D-pad navigable picker. Returns the chosen index, or -1 on
 * Triangle-abort. With one ISO we skip the menu and return 0; with
 * no pad attached we also fall back to 0 silently. */
int  pick_iso(char isos[MAX_ISOS][280], int count);

#endif
