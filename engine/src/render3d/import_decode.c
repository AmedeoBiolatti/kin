/* What the material import decodes that kin's images (SDL_image) don't (render3d/image_decode3d.cpp): Photoshop's
 * .psd (its composited picture) and zlib streams (TIFF's Deflate, cooked material graphs), by stb_image
 * (github.com/nothings/stb), compiled once as C without kin's warnings, static (beside hdr_decode.c's), with only PSD;
 * and zlib streams written (cooked material graphs: render3d/material_graph3d.cpp) by stb_image_write. */
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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* `bytes` as a zlib stream (free with kin_deflate_free); null if it can't be made. */
unsigned char* kin_zlib_deflate(const unsigned char* bytes, int size, int* out_size) {
    return stbi_zlib_compress((unsigned char*)bytes, size, out_size, 32);
}

void kin_deflate_free(void* bytes) {
    STBIW_FREE(bytes);
}
