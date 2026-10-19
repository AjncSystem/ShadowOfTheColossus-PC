// sotc port: stb_image_write implementation for the presenter screenshots, and stb_image (PNG
// only) for the controller glyph replacements (texture_cache.cpp, SOTC_TEX_REPLACE).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
