#include "PngIo.h"

#include <string>
#include <stdexcept>

#include <png.h>


static const NvU32 kBpp = 4;


// The simplified libpng API (PNG_IMAGE_*) gives us format conversion,
// orientation, and alpha handling for free. PNG_FORMAT_BGRA matches this
// repo's A8R8G8B8-as-little-endian-NvU32 memory layout, and the simplified
// reader/writer are already top-down, so callers never see flipped rows.

void ReadPngA8R8G8B8(const char *path, std::vector<NvU8> &outRgba,
	NvU32 &outWidth, NvU32 &outHeight)
{
	png_image img = {};
	img.version = PNG_IMAGE_VERSION;

	if (png_image_begin_read_from_file(&img, path) == 0) {
		throw std::runtime_error(std::string("opening PNG ") + path
			+ ": " + img.message);
	}

	// Force 8-bit BGRA regardless of the source color type / bit depth.
	img.format = PNG_FORMAT_BGRA;

	if (PNG_IMAGE_PIXEL_SIZE(img.format) != kBpp) {
		png_image_free(&img);
		throw std::runtime_error("PNG: unexpected pixel size");
	}

	outRgba.resize((size_t)PNG_IMAGE_SIZE(img));

	// row_stride 0 lets libpng use the natural width*4 stride; negative
	// would flip to bottom-up, which we don't want.
	if (png_image_finish_read(&img, nullptr, outRgba.data(), 0, nullptr) == 0) {
		std::string msg = img.message;
		png_image_free(&img);
		throw std::runtime_error(std::string("reading PNG ") + path + ": " + msg);
	}

	outWidth  = img.width;
	outHeight = img.height;
}


void WritePngA8R8G8B8(const char *path, NvU32 width, NvU32 height, const NvU8 *pixels)
{
	png_image img = {};
	img.version = PNG_IMAGE_VERSION;
	img.width   = width;
	img.height  = height;
	img.format  = PNG_FORMAT_BGRA;

	// row_stride 0 => width*4; convert_to_8bit 0 since we already are 8-bit.
	if (png_image_write_to_file(&img, path, 0, pixels, 0, nullptr) == 0) {
		throw std::runtime_error(std::string("writing PNG ") + path
			+ ": " + img.message);
	}
}
