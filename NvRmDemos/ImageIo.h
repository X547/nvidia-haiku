#pragma once

#include <vector>

extern "C" {
#include "nvtypes.h"
}


// Format-dispatching wrappers over BmpIo / PngIo. The container is chosen
// from the path's extension (case-insensitive ".png" -> PNG, anything else
// -> BMP). Pixels are top-down A8R8G8B8 in both directions, exactly as the
// underlying readers/writers expect. PNG preserves alpha; BMP forces opaque
// on write (see BmpIo.h).


void ReadImageA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight);

void WriteImageA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels);
