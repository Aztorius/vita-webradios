#include <mpg123.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/clib.h>

#include "audio.h"

#define printf sceClibPrintf

static mpg123_handle *mp3;
static off_t frames_read = 0, total_samples = 0;
static long sample_rate = 0;
static int channels = 0;
static SceBool playing = SCE_FALSE;


SceBool MP3_playing() {
	return playing;
}

int MP3_Init() {
	int error = mpg123_init();
	if (error != MPG123_OK)
		return error;

	mp3 = mpg123_new(NULL, &error);
	if (error != MPG123_OK)
		return error;

	error = mpg123_param(mp3, MPG123_FLAGS, MPG123_FORCE_SEEKABLE | MPG123_FUZZY | MPG123_SEEKBUFFER | MPG123_GAPLESS, 0.0);
	if (error != MPG123_OK)
		return error;

	// Let the seek index auto-grow and contain an entry for every frame
	error = mpg123_param(mp3, MPG123_INDEX_SIZE, -1, 0.0);
	if (error != MPG123_OK)
		return error;

	error = mpg123_param(mp3, MPG123_ADD_FLAGS, MPG123_PICTURE, 0.0);
	if (error != MPG123_OK)
		return error;

	error = mpg123_open_feed(mp3);
	if (error != MPG123_OK)
		return error;

	return 0;
}

SceUInt32 MP3_GetSampleRate(void) {
	return sample_rate;
}

SceUInt8 MP3_GetChannels(void) {
	return channels;
}

int MP3_Feed(void *inbuf, unsigned int inlength) {
	int ret = 0;

	ret = mpg123_feed(mp3, inbuf, inlength);
	if (ret == MPG123_ERR) {
		printf("MP3_Decode error: %s\n", mpg123_strerror(mp3));
	}

	return ret;
}

int MP3_Decode(void *inbuf, unsigned int inlength, void *outbuf, unsigned int outlength, unsigned int *sizeout) {
	int ret = 0;

	ret = mpg123_decode(mp3, inbuf, inlength, outbuf, outlength, sizeout);

	if (ret == MPG123_NEW_FORMAT) {
		int enc;
		mpg123_getformat(mp3, &sample_rate, &channels, &enc);
	} else if (ret == MPG123_ERR) {
		printf("MP3_Decode error: %s\n", mpg123_strerror(mp3));
	} else if (ret == MPG123_NEED_MORE) {
		printf("MP3_Decode needs more data\n");
	}

	return ret;
}

SceUInt64 MP3_GetPosition(void) {
	return frames_read;
}

SceUInt64 MP3_GetLength(void) {
	return total_samples;
}

SceUInt64 MP3_Seek(SceUInt64 index) {
	off_t seek_frame = (total_samples * (index / 450.0));
	
	if (mpg123_seek(mp3, seek_frame, SEEK_SET) >= 0) {
		frames_read = seek_frame;
		return frames_read;
	}

	return -1;
}

void MP3_Term(void) {
	frames_read = 0;
	
	if (metadata.has_meta)
		metadata.has_meta = SCE_FALSE;
	
	mpg123_close(mp3);
	mpg123_delete(mp3);
	mpg123_exit();
}
