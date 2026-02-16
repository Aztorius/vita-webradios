#include "utils.hpp"

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>

#define printf sceClibPrintf


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

bool is_samplerate_vita_compatible(int samplerate)
{
	int compatible_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
	int samplerate_is_compatible = false;
	int nsamples = 0;

	for (int i = 0; i < sizeof(compatible_freqs); i++) {
		if (samplerate == compatible_freqs[i]) {
			samplerate_is_compatible = true;
			break;
		}
	}

	return samplerate_is_compatible;
}
