// tinyexr's implementation (BSD-3), built without our warning flags (third-party code). It inflates with
// stb_image's zlib (stb_impl.c) instead of bringing miniz.
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
