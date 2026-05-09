/*
 * Hello-world EE ELF that gets wrapped into test.iso and installed
 * via ps2-usbhdl. If OPL launches the resulting HDL partition and
 * this text appears, the entire install pipeline (create + format
 * + stream + boot) is working.
 */
#include <kernel.h>
#include <debug.h>

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	init_scr();
	scr_printf("\n");
	scr_printf("  ps2-usbhdl test ISO booted via HDL!\n");
	scr_printf("  build: " __DATE__ " " __TIME__ "\n");
	scr_printf("\n");
	scr_printf("  the install pipeline (create + format + stream)\n");
	scr_printf("  produced a partition that OPL launched and ran.\n");
	scr_printf("\n");
	scr_printf("  power-cycle to return.\n");

	SleepThread();
	return 0;
}
