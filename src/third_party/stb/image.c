// ps5-homebrew-ui - The single compilation unit for the vendored stb_image.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PNG, JPEG and GIF are decoded (Flashpoint logos are PNG; the websites' list
// pictures are JPEG or GIF), from memory: images are read into memory first, so
// stdio is left out. (Flashpoint PS5 change: JPEG and GIF added.)

#define STB_IMAGE_IMPLEMENTATION 1
#define STBI_ONLY_PNG 1
#define STBI_ONLY_JPEG 1
#define STBI_ONLY_GIF 1
#define STBI_NO_STDIO 1
#include "stb_image.inc"
