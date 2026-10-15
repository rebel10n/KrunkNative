/* The stb_image implementation for kn_brain's profile pictures (avatar_image.c decodes PNG / JPEG
 * with it; the header is client/include/stb_image.h, public domain). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
