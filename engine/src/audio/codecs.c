/* The audio decoders' implementations, compiled once as C: dr_wav, dr_flac and
 * dr_mp3 (github.com/mackron/dr_libs) and stb_vorbis (github.com/nothings/stb).
 * audio_decoder.cpp includes their declarations. */
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
