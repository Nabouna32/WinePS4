/* WinePS4: the stb libraries the canvas uses (pictures, fonts), compiled once. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "third_party/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
