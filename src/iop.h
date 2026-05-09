#ifndef PS2USBHDL_IOP_H
#define PS2USBHDL_IOP_H

/* Reset the IOP, apply SBV LMB patches, then load the full module
 * stack (iomanX, fileXio, poweroff, ps2dev9, ps2atad, ps2hdd-hdl,
 * hdlfs, usbd, bdm, bdmfs_fatfs, usbmass_bd, sio2man, padman) plus
 * fileXioInit + poweroffInit. Prints "IOP modules: all 13 ok" or
 * a per-failure FAIL line on the way through. */
void boot_iop_with_modules(void);

#endif
