#include <debug.h>
#include <delaythread.h>
#include <kernel.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

void delay_ms(int ms) { DelayThread(ms * 1000); }

/* Pad state buffer. libpad requires 256 bytes, 64-byte aligned. */
static char pad_buf[256] __attribute__((aligned(64)));
static int pad_inited = 0;

int pad_init(void) {
  if (pad_inited)
    return 1;

  padInit(0);
  if (padPortOpen(0, 0, pad_buf) == 0)
    return 0;

  int retries = 30; /* ~3 seconds at 100 ms each */
  while (retries-- > 0) {
    int state = padGetState(0, 0);
    if (state == PAD_STATE_STABLE) {
      pad_inited = 1;
      return 1;
    }
    if (state == PAD_STATE_DISCONN)
      return 0;
    delay_ms(100);
  }
  return 0;
}

int pick_mode(void) {
  if (!pad_init())
    return -1;

  static const char *labels[] = {
      "install games from USB",
      "manage installed games",
      "exit",
  };
  enum { COUNT = 3 };

  int idx = 0;
  int prev_pressed = 0;
  int menu_y = scr_getY();
  int dirty = 1;

  for (;;) {
    if (dirty) {
      int i;
      for (i = 0; i < COUNT; i++) {
        scr_clearline(menu_y + i);
        scr_setXY(0, menu_y + i);
        scr_printf("    %c %s", i == idx ? '>' : ' ', labels[i]);
      }
      scr_clearline(menu_y + COUNT);
      scr_setXY(0, menu_y + COUNT);
      scr_printf("    [D-pad] move  [X] select  [/\\] exit");
      dirty = 0;
    }

    struct padButtonStatus pad;
    if (padRead(0, 0, &pad) != 0) {
      int pressed = (~pad.btns) & 0xFFFF;
      int newly = pressed & ~prev_pressed;
      prev_pressed = pressed;

      if (newly & PAD_UP) {
        idx = (idx + COUNT - 1) % COUNT;
        dirty = 1;
      }
      if (newly & PAD_DOWN) {
        idx = (idx + 1) % COUNT;
        dirty = 1;
      }
      if (newly & PAD_CROSS) {
        scr_setXY(0, menu_y + COUNT + 1);
        return idx == 2 ? -1 : idx; /* 0=install, 1=manage, 2=exit */
      }
      if (newly & PAD_TRIANGLE) {
        scr_setXY(0, menu_y + COUNT + 1);
        return -1;
      }
    }
    delay_ms(50);
  }
}

int pick_items(char items[][280], int count, int selected[]) {
  int i;
  for (i = 0; i < count; i++)
    selected[i] = 0;

  if (count == 0)
    return 0;
  if (count == 1) {
    selected[0] = 1;
    return 1;
  }

  if (!pad_init()) {
    scr_printf("  no pad - selecting first only: %s\n", items[0]);
    selected[0] = 1;
    return 1;
  }

  int idx = 0;
  int prev_pressed = 0;
  int menu_y = scr_getY();
  int dirty = 1;

  for (;;) {
    if (dirty) {
      for (i = 0; i < count; i++) {
        scr_clearline(menu_y + i);
        scr_setXY(0, menu_y + i);
        scr_printf("    %c [%c] %s", i == idx ? '>' : ' ',
                   selected[i] ? '*' : ' ', items[i]);
      }
      scr_clearline(menu_y + count);
      scr_setXY(0, menu_y + count);
      scr_printf("    [D-pad]  [#] toggle  [X] start  [/\\] abort");
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
      if (newly & PAD_SQUARE) {
        selected[idx] = !selected[idx];
        dirty = 1;
      }
      if (newly & PAD_CROSS) {
        int n = 0;
        for (i = 0; i < count; i++)
          if (selected[i])
            n++;
        /* If user hit X without toggling anything,
         * treat the current row as the single
         * selection — covers the "I just want this
         * one" case without forcing a Square press. */
        if (n == 0) {
          selected[idx] = 1;
          n = 1;
        }
        scr_setXY(0, menu_y + count + 1);
        return n;
      }
      if (newly & PAD_TRIANGLE) {
        scr_setXY(0, menu_y + count + 1);
        return 0;
      }
    }
    delay_ms(50);
  }
}
