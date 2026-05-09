#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <delaythread.h>
#include <debug.h>
#include <libpad.h>

#include "ui.h"

void delay_ms(int ms)
{
	DelayThread(ms * 1000);
}

/* Pad state buffer. libpad requires 256 bytes, 64-byte aligned. */
static char pad_buf[256] __attribute__((aligned(64)));

/* Bring up port 0 / slot 0 and wait for the pad to reach STABLE.
 * Returns 1 on success, 0 if no pad is connected within timeout. */
static int pad_init_port0(void)
{
	padInit(0);
	if (padPortOpen(0, 0, pad_buf) == 0)
		return 0;

	int retries = 100; /* ~10 seconds at 100 ms each */
	while (retries-- > 0) {
		int state = padGetState(0, 0);
		if (state == PAD_STATE_STABLE) return 1;
		if (state == PAD_STATE_DISCONN) return 0;
		delay_ms(100);
	}
	return 0;
}

int pick_iso(char isos[MAX_ISOS][280], int count)
{
	if (count <= 1) return count == 1 ? 0 : -1;

	if (!pad_init_port0()) {
		scr_printf("  no pad - using %s\n", isos[0]);
		return 0;
	}

	int idx = 0;
	int prev_pressed = 0;
	int menu_y = scr_getY();
	int dirty = 1;

	for (;;) {
		if (dirty) {
			int i;
			for (i = 0; i < count; i++) {
				scr_clearline(menu_y + i);
				scr_setXY(0, menu_y + i);
				scr_printf("    %c %s",
				           i == idx ? '>' : ' ', isos[i]);
			}
			scr_clearline(menu_y + count);
			scr_setXY(0, menu_y + count);
			scr_printf("    [D-pad] move  [X] install  [/\\] abort");
			dirty = 0;
		}

		struct padButtonStatus pad;
		if (padRead(0, 0, &pad) != 0) {
			int pressed = (~pad.btns) & 0xFFFF;
			int newly = pressed & ~prev_pressed;
			prev_pressed = pressed;

			if (newly & PAD_UP) {
				idx = (idx + count - 1) % count;
				dirty = 1;
			}
			if (newly & PAD_DOWN) {
				idx = (idx + 1) % count;
				dirty = 1;
			}
			if (newly & PAD_CROSS) {
				scr_setXY(0, menu_y + count + 1);
				return idx;
			}
			if (newly & PAD_TRIANGLE) {
				scr_setXY(0, menu_y + count + 1);
				return -1;
			}
		}
		delay_ms(50);
	}
}
