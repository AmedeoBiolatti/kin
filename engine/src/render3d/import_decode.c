/* What the material import decodes that kin's images (SDL_image) don't (render3d/image_decode3d.cpp): Photoshop's
 * .psd (its composited picture) and zlib streams (TIFF's Deflate), by stb_image (github.com/nothings/stb), compiled
 * once as C without kin's warnings, static (beside hdr_decode.c's), with only PSD. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PSD
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "stb_image.h"

/* RGBA bytes, rows top to bottom; free with kin_import_free. Null if it isn't one. */
unsigned char* kin_psd_load(const unsigned char* bytes, int size, int* width, int* height) {
    int channels = 0;
    return stbi_load_from_memory(bytes, size, width, height, &channels, 4);
}

/* A zlib stream inflated (`expected` bytes a hint); free with kin_import_free. Null if it isn't one. */
char* kin_zlib_inflate(const char* bytes, int size, int expected, int* out_size) {
    return stbi_zlib_decode_malloc_guesssize_headerflag(bytes, size, expected > 0 ? expected : size * 4, out_size, 1);
}

void kin_import_free(void* pixels) {
    stbi_image_free(pixels);
}
