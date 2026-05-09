#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <debug.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <hdd-ioctl.h>
#include <libhdd.h>

#include "hdl.h"

/* Mirror of HDLGameInstaller's hdlfs/hdlfs.h struct HDLFS_FormatArgs.
 * Passed to fileXioFormat("hdl0:", partname, &args, sizeof(args))
 * after the partition has been created. Layout must match exactly
 * — wrong padding here = wrong on-disk header. */
#define HDLFS_GAME_TITLE_LEN  160
#define HDLFS_STARTUP_PTH_LEN 60

struct HDLFS_FormatArgs {
	uint8_t  CompatFlags;
	uint8_t  DiscType;       /* 0x12 = CD, 0x14 = DVD */
	uint8_t  TRType;
	uint8_t  TRMode;
	uint32_t NumSectors;     /* total 2 KB sectors of disc data */
	uint32_t Layer1Start;    /* sector offset of layer 1 for DVD9 */
	char     GameTitle[HDLFS_GAME_TITLE_LEN];
	char     StartupPath[HDLFS_STARTUP_PTH_LEN];
};

void show_hdd(void)
{
	if (hddCheckPresent() != 0 || hddCheckFormatted() != 0) {
		scr_printf("\n  HDD: not present or not APA-formatted\n");
		return;
	}

	t_hddInfo info;
	hddGetInfo(&info);
	scr_printf("\n  HDD: %lu MB free of %lu MB (max-part %lu MB)\n",
	           (unsigned long)info.hddFree,
	           (unsigned long)info.hddSize,
	           (unsigned long)info.hddMaxPartitionSize);

	t_hddFilesystem fs[16];
	int n = hddGetFilesystemList(fs, 16);
	scr_printf("  PFS parts (%d):", n);
	int i;
	for (i = 0; i < n && i < 4; i++)
		scr_printf(" %s%s", fs[i].name, i + 1 < n && i + 1 < 4 ? "," : "");
	if (n > 4) scr_printf(" ...");
	scr_printf("\n");
}

int partition_exists(const char *target)
{
	int dd = fileXioDopen("hdd0:");
	if (dd < 0) return -1;

	iox_dirent_t de;
	int found = 0;
	while (fileXioDread(dd, &de) > 0) {
		if (strcmp(de.name, target) == 0) {
			found = 1;
			break;
		}
	}
	fileXioDclose(dd);
	return found;
}

int partition_remove(const char *target)
{
	char hdd_path[64];
	snprintf(hdd_path, sizeof(hdd_path), "hdd0:%s", target);
	return fileXioRemove(hdd_path);
}

int list_hdl_partitions(char names[][33], uint32_t sizes_mb[], int max)
{
	int dd = fileXioDopen("hdd0:");
	if (dd < 0) return -1;

	iox_dirent_t de;
	int n = 0;
	while (n < max && fileXioDread(dd, &de) > 0) {
		if (de.stat.mode != APA_TYPE_HDL) continue;
		/* APA names cap at 32 chars; copy with explicit null. */
		memcpy(names[n], de.name, 32);
		names[n][32] = 0;
		/* dirent size is in 512-byte sectors; /2048 -> MB. */
		sizes_mb[n] = de.stat.size / 2048;
		n++;
	}
	fileXioDclose(dd);
	return n;
}

int execute_install(const install_plan_t *plan)
{
	char hdd_path[64];      /* hdd0:<name> */
	char create_cmd[80];    /* hdd0:<name>,,,SIZE,HDL */
	snprintf(hdd_path,   sizeof(hdd_path),   "hdd0:%s",
	         plan->partition_name);
	snprintf(create_cmd, sizeof(create_cmd), "%s,,,%s,HDL",
	         hdd_path, plan->main_size_str);

	scr_printf("  fileXioOpen: %s\n", create_cmd);
	int fd = fileXioOpen(create_cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		scr_printf("  open failed: %d\n", fd);
		return -1;
	}

	int i;
	for (i = 0; i < plan->subs_needed; i++) {
		const char *sz = plan->sub_size_strs[i];
		scr_printf("  add sub %d/%d (%s)\n",
		           i + 1, plan->subs_needed, sz);
		int sret = fileXioIoctl2(fd, HIOCADDSUB,
		                         (char *)sz, strlen(sz) + 1,
		                         NULL, 0);
		if (sret < 0) {
			scr_printf("  add_sub %d failed: %d\n", i + 1, sret);
			fileXioClose(fd);
			fileXioRemove(hdd_path);
			return -1;
		}
	}
	fileXioClose(fd);

	struct HDLFS_FormatArgs args;
	memset(&args, 0, sizeof(args));
	args.DiscType    = plan->disc_type;
	args.NumSectors  = plan->iso_sectors_2k;
	args.Layer1Start = plan->layer1_start;
	strncpy(args.GameTitle,   plan->volume_id,
	        sizeof(args.GameTitle) - 1);
	strncpy(args.StartupPath, plan->startup_id,
	        sizeof(args.StartupPath) - 1);

	scr_printf("  fileXioFormat: %s\n", hdd_path);
	int ret = fileXioFormat("hdl0:", hdd_path,
	                        (const char *)&args, sizeof(args));
	if (ret < 0) {
		scr_printf("  format failed: %d (removing partition)\n", ret);
		fileXioRemove(hdd_path);
		return -1;
	}

	return 0;
}

/* Render a fixed-width [###  ] progress bar into out. */
static void render_bar(char *out, int width, int pct)
{
	int inner = width - 2;
	int filled = (pct * inner) / 100;
	out[0] = '[';
	int i;
	for (i = 0; i < inner; i++)
		out[1 + i] = (i < filled) ? '#' : ' ';
	out[width - 1] = ']';
	out[width] = 0;
}

int stream_iso_to_partition(const install_plan_t *plan,
                            const char *iso_path)
{
	char hdd_path[64];
	snprintf(hdd_path, sizeof(hdd_path), "hdd0:%s", plan->partition_name);

	uint64_t total = (uint64_t)plan->iso_sectors_2k * 2048;
	unsigned total_mb = (unsigned)(total >> 20);

	/* Clear the screen before streaming so the progress bar
	 * doesn't fight with prior output (the 27-line debug screen
	 * wraps to (0,0) and the bar ends up overwriting the banner). */
	scr_clear();
	scr_setXY(0, 0);
	scr_printf("\n  installing  %s\n", plan->partition_name);
	scr_printf("  source      %s\n", iso_path);
	scr_printf("  size        %u MB (~%u min at USB 1.1)\n\n",
	           total_mb, (total_mb + 59) / 60);

	scr_printf("  fileXioMount    ");
	int ret = fileXioMount("hdl0:", hdd_path, FIO_MT_RDWR);
	if (ret < 0) {
		scr_printf("FAIL %d\n", ret);
		return -1;
	}
	scr_printf("ok\n");

	scr_printf("  fileXioOpen     ");
	int hdl_fd = fileXioOpen("hdl0:", O_RDWR);
	if (hdl_fd < 0) {
		scr_printf("FAIL %d\n", hdl_fd);
		fileXioUmount("hdl0:");
		return -1;
	}
	scr_printf("ok\n");

	scr_printf("  open ISO        ");
	int iso_fd = open(iso_path, O_RDONLY);
	if (iso_fd < 0) {
		scr_printf("FAIL %d\n", iso_fd);
		fileXioClose(hdl_fd);
		fileXioUmount("hdl0:");
		return -1;
	}
	scr_printf("ok\n\n");

	/* 1 MB chunks: a sweet spot between USB-1.1 throughput
	 * (≈1 MB/s) and EE RAM. */
	static uint8_t buf[1024 * 1024] __attribute__((aligned(64)));

	uint64_t written = 0;
	int last_pct = -1;
	time_t last_render = 0;
	time_t start_t = time(NULL);
	int progress_y = scr_getY();

	while (written < total) {
		uint32_t want = (total - written > sizeof(buf))
		                ? (uint32_t)sizeof(buf)
		                : (uint32_t)(total - written);
		int got = read(iso_fd, buf, want);
		if (got <= 0) {
			scr_printf("\n  read err at %u MB: %d\n",
			           (unsigned)(written >> 20), got);
			break;
		}
		int wrote = fileXioWrite(hdl_fd, buf, got);
		if (wrote != got) {
			scr_printf("\n  write err at %u MB: %d/%d\n",
			           (unsigned)(written >> 20), wrote, got);
			break;
		}
		written += wrote;

		int pct = (int)((written * 100) / total);
		time_t now = time(NULL);
		if (pct != last_pct || now != last_render) {
			time_t elapsed = now - start_t;
			unsigned eta = 0;
			unsigned mbps10 = 0; /* MB/s × 10 */
			if (elapsed > 0 && written > 0) {
				uint64_t remaining = total - written;
				eta = (unsigned)((remaining * (uint64_t)elapsed)
				                 / written);
				mbps10 = (unsigned)((written * 10ULL) >> 20)
				         / (unsigned)elapsed;
			}

			char bar[31];
			render_bar(bar, 30, pct);

			scr_clearline(progress_y);
			scr_setXY(0, progress_y);
			scr_printf("  %s %3d%%", bar, pct);

			scr_clearline(progress_y + 1);
			scr_setXY(0, progress_y + 1);
			scr_printf("  %u/%u MB  %u.%u MB/s  %u:%02u elapsed  ETA %u:%02u",
			           (unsigned)(written >> 20), total_mb,
			           mbps10 / 10, mbps10 % 10,
			           (unsigned)(elapsed / 60),
			           (unsigned)(elapsed % 60),
			           eta / 60, eta % 60);
			last_pct = pct;
			last_render = now;
		}
	}

	close(iso_fd);
	fileXioClose(hdl_fd);
	fileXioUmount("hdl0:");

	scr_setXY(0, progress_y + 3);
	if (written == total) {
		time_t elapsed = time(NULL) - start_t;
		scr_printf("  install complete in %u:%02u — boot via OPL\n",
		           (unsigned)(elapsed / 60),
		           (unsigned)(elapsed % 60));
		return 0;
	}
	scr_printf("  install incomplete: %u/%u MB written\n",
	           (unsigned)(written >> 20), total_mb);
	return -1;
}
