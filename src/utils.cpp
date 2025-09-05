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


char *icy_parse_stream_title(char *data, int size)
{
	// Find "StreamTitle='XXX'" in data and replace data value with "XXX\0"
	if (size <= 13) {
		sceClibPrintf("ICY Metadata to short to find stream title\n");
		return NULL;
	}

	char *title = (char*)malloc(size);
	if (!title) {
		sceClibPrintf("Cannot allocate title\n");
		return NULL;
	}

	memcpy(title, data, size);
	title[size-1] = '\0';

	char *result_left = strstr(title, "StreamTitle='");
	if (!result_left) {
		sceClibPrintf("Cannot find \"StreamTitle='\"\n");
		free(title);
		return NULL;
	}

	char *result_right = strstr(title + 13, "'");
	if (!result_right) {
		sceClibPrintf("Cannot find \"'\"\n");
		free(title);
		return NULL;
	}

	int title_size = result_right - (result_left + 13) + 1;
	memmove(title, result_left + 13, title_size);
	title[title_size-1] = '\0';
	return title;
}
