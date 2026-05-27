#include "ImageIo.h"

#include "BmpIo.h"
#include "PngIo.h"

#include <strings.h>
#include <string.h>


static bool HasPngExtension(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot != nullptr && strcasecmp(dot, ".png") == 0;
}


void ReadImageA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight)
{
	if (HasPngExtension(path)) {
		ReadPngA8R8G8B8(path, outRgba, outWidth, outHeight);
	} else {
		ReadBmpA8R8G8B8(path, outRgba, outWidth, outHeight);
	}
}


void WriteImageA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels)
{
	if (HasPngExtension(path)) {
		WritePngA8R8G8B8(path, width, height, pixels);
	} else {
		WriteBmpA8R8G8B8(path, width, height, pixels);
	}
}
