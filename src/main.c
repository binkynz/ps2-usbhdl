#include <stdio.h>
#include <kernel.h>
#include <debug.h>

int main(int argc, char *argv[])
{
	init_scr();
	scr_printf("\n\n  ps2-usbhdl: hello from PS2\n");
	scr_printf("  toolchain build OK\n");
	scr_printf("  built with %s\n\n", __VERSION__);
	scr_printf("  power-cycle the console to return.\n");

	SleepThread();
	return 0;
}
