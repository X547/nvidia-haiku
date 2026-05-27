#pragma once

#include <vector>

extern "C" {
#include "nvtypes.h"
}


// Minimal 32-bit PNG I/O mirroring BmpIo. Pixels are A8R8G8B8 (BGRA byte
// order in memory, equivalent to NV902D_SET_*_FORMAT_V_A8R8G8B8 when read
// as a little-endian NvU32). Arrays are top-down: row 0 is the top row of
// the image, row N-1 the bottom -- matching libpng's native orientation,
// so no row flipping happens here. Unlike the BMP writer, alpha is carried
// through verbatim in both directions; nothing is forced opaque.


// Read a PNG into `outRgba` (top-down A8R8G8B8). Any source color type /
// bit depth is converted to 8-bit BGRA. Returns the image dimensions via
// the out-params. Throws std::runtime_error on I/O failure.
void ReadPngA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight);


// Write a PNG from `pixels` (top-down A8R8G8B8, width*height*4 bytes). The
// per-pixel alpha byte is preserved, so partially transparent pixels stay
// transparent in the file. Throws std::runtime_error on I/O failure.
void WritePngA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels);
