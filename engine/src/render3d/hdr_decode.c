/* Radiance .hdr pictures (render3d/environment.cpp), by stb_image (github.com/nothings/stb),
 * compiled once as C without kin's warnings and with only that format. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#include "stb_image.h"

/* Linear RGB floats, 3 a pixel, rows top to bottom; free with kin_hdr_free. Null if it isn't one. */
float* kin_hdr_load(const unsigned char* bytes, int size, int* width, int* height) {
    int channels = 0;
    return stbi_loadf_from_memory(bytes, size, width, height, &channels, 3);
}

void kin_hdr_free(float* pixels) {
    stbi_image_free(pixels);
}
