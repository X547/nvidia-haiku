#include "BmpIo.h"

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include <system_error>
#include <stdexcept>
#include <string>

#include <ErrorUtils.h>
#include <FileDesc.h>


static const NvU32 kBpp = 4;


void ReadBmpA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight)
{
	FileDesc fd(open(path, O_RDONLY | O_CLOEXEC));
	if (!fd.IsSet()) {
		throw std::system_error(errno, std::generic_category(),
			std::string("opening ") + path);
	}

	// File header (14) + INFOHEADER (40). V4 / V5 headers are 108 / 124
	// bytes; we only need the fields that overlap with INFOHEADER, plus
	// the data offset from the file header.
	NvU8 hdr[14 + 124];
	ssize_t got = read(fd.Get(), hdr, sizeof(hdr));
	if (got < (ssize_t)(14 + 40)) {
		throw std::runtime_error("BMP truncated");
	}
	if (hdr[0] != 'B' || hdr[1] != 'M') {
		throw std::runtime_error("not a BMP file");
	}

	NvU32 dataOff = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
	NvU32 width   = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
	NvS32 height  = (NvS32)(hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
	NvU16 bpp     = hdr[28] | (hdr[29] << 8);
	bool topDown  = (height < 0);
	NvU32 absHeight = (NvU32)(topDown ? -height : height);

	if (bpp != 32) {
		throw std::runtime_error("BMP must be 32 bpp");
	}

	if (lseek(fd.Get(), dataOff, SEEK_SET) == (off_t)-1) {
		CheckErrno(-1);
	}

	const NvU32 stride = width * kBpp;
	outRgba.resize((size_t)stride * absHeight);

	// BMP stores rows bottom-up unless `height` is negative. Read row
	// by row into the destination so we can place each row at its
	// top-down position without an extra full-image copy.
	for (NvU32 r = 0; r < absHeight; r++) {
		NvU32 dstRow = topDown ? r : (absHeight - 1 - r);
		ssize_t n = read(fd.Get(), outRgba.data() + (size_t)dstRow * stride, stride);
		if (n != (ssize_t)stride) {
			throw std::runtime_error("BMP read truncated");
		}
	}

	outWidth  = width;
	outHeight = absHeight;
}


void WriteBmpA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels)
{
	const NvU32 stride = width * kBpp;
	const NvU32 imgBytes = stride * height;
	const NvU32 fileSize = 14 + 40 + imgBytes;

	NvU8 hdr[14 + 40] = {};
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (NvU8)(fileSize); hdr[3] = (NvU8)(fileSize >> 8);
	hdr[4] = (NvU8)(fileSize >> 16); hdr[5] = (NvU8)(fileSize >> 24);
	hdr[10] = 54; // pixel data offset

	hdr[14] = 40; // INFOHEADER size
	hdr[18] = (NvU8)(width); hdr[19] = (NvU8)(width >> 8);
	hdr[20] = (NvU8)(width >> 16); hdr[21] = (NvU8)(width >> 24);
	hdr[22] = (NvU8)(height); hdr[23] = (NvU8)(height >> 8);
	hdr[24] = (NvU8)(height >> 16); hdr[25] = (NvU8)(height >> 24);
	hdr[26] = 1;  // planes
	hdr[28] = 32; // bpp
	// compression = BI_RGB (0); sizeImage may be 0 for BI_RGB.

	FileDesc fd(open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
	if (!fd.IsSet()) {
		throw std::system_error(errno, std::generic_category(),
			std::string("creating ") + path);
	}
	if (write(fd.Get(), hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
		throw std::runtime_error("BMP header write failed");
	}

	// BMP wants rows bottom-up. Force alpha to 0xFF per pixel as we go
	// so the file is opaque regardless of what the blits left in that
	// byte (BLEND_PREMULT can leave fractional alpha in the canvas).
	std::vector<NvU8> row(stride);
	for (NvS32 r = (NvS32)height - 1; r >= 0; r--) {
		memcpy(row.data(), pixels + (size_t)r * stride, stride);
		for (NvU32 x = 0; x < width; x++) {
			row[x * 4 + 3] = 0xFF;
		}
		if (write(fd.Get(), row.data(), stride) != (ssize_t)stride) {
			throw std::runtime_error("BMP pixel write failed");
		}
	}
}
