#include "utils.hpp"

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h> 
#include <psp2/kernel/threadmgr.h>
#include <psp2/shellutil.h>

#define printf sceClibPrintf

static int lock_power = 0;

int copyfile(const char *destfile, const char *srcfile)
{
	FILE *fout = fopen(destfile, "wb");
	if (!fout) {
		printf("Cannot create file %s\n", destfile);
		return -1;
	}

	FILE *fin = fopen(srcfile, "rb");
	if (!fin) {
		printf("Cannot open file %s\n", srcfile);
		return -1;
	}

	char buf[512];
	int nread = 0;

	do {
		nread = fread(buf, 1, 512, fin);
		if (nread > 0) {
			fwrite(buf, 1, nread, fout);
		}
	} while (nread > 0);

	fclose(fin);
	fclose(fout);
	return 0;
}

static int power_tick_thread(SceSize args, void *argp) {
	while (1) {
		if (lock_power > 0) {
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
		}

		sceKernelDelayThread(5 * 1000 * 1000);
	}
	return 0;
}

void Utils_InitPowerTick(void) {
	SceUID thid = 0;
	thid = sceKernelCreateThread("power_tick_thread", power_tick_thread, 0x10000100, 0x40000, 0, 0, NULL);
	if (thid > 0)
		sceKernelStartThread(thid, 0, NULL);
}

void Utils_LockPower(void) {
	if (!lock_power)
		sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);

	lock_power++;
}

void Utils_UnlockPower(void) {
	if (lock_power)
		sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);

	lock_power--;
	if (lock_power < 0)
		lock_power = 0;
}
