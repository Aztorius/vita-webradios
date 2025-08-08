#ifndef _ELEVENMPV_AUDIO_MP3_H_
#define _ELEVENMPV_AUDIO_MP3_H_

#include <psp2/types.h>

int MP3_Init(void);
SceUInt32 MP3_GetSampleRate(void);
SceUInt8 MP3_GetChannels(void);
int MP3_FirstDecode(void *inbuf, unsigned int inlength, void *outbuf, unsigned int outlength);
int MP3_Decode(void *inbuf, unsigned int inlength, void *outbuf, unsigned int outlength, unsigned int *sizeout);
SceUInt64 MP3_GetPosition(void);
SceUInt64 MP3_GetLength(void);
SceUInt64 MP3_Seek(SceUInt64 index);
void MP3_Term(void);
SceBool MP3_playing(void);

#endif
