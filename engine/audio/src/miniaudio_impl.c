// The miniaudio implementation, with OGG Vorbis through the stb_vorbis copy miniaudio ships
// (both public domain / MIT-0). Compiled without our warning flags: third-party code.
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
