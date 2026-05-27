#pragma once

#include <vector>

extern "C" {
#include "nvtypes.h"
}


// Minimal 32-bit BMP I/O for the 2D blit demo. Only A8R8G8B8 (BGRA byte
// order in the file, equivalent to NV902D_SET_*_FORMAT_V_A8R8G8B8 when
// read as a little-endian NvU32) is supported. Pixel arrays are
// top-down: row 0 is the top row of the image, row N-1 is the bottom.
// The file format itself is bottom-up by default; readers/writers in
// here transparently flip rows so callers never see bottom-up data.


// Read a 32-bit BMP into `outRgba` (top-down A8R8G8B8). Returns the
// image dimensions via the out-params. Throws std::system_error /
// std::runtime_error on I/O failure or unsupported pixel layout.
void ReadBmpA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight);


// Write a 32-bit BMP from `pixels` (top-down A8R8G8B8, width*height*4
// bytes). The output uses a BITMAPINFOHEADER with compression=BI_RGB
// and forces alpha to 0xFF in every pixel so viewers that honor the
// alpha byte don't render the file as transparent.
void WriteBmpA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels);
