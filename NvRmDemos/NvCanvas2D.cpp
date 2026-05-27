// NvCanvas2D — a thin record-only wrapper over FERMI_TWOD_A.
//
// Every method here just appends pushbuffer methods to the caller's
// NvCommandBuffer; the exact method sequences mirror the ones validated in
// NvRm2dBlitDemo (TwodSetupDst / TwodSetupSrc / Twod2dBlit /
// TwodSetupBlendConstants), generalized for the Bitmap format/layout and for
// the solid-prim fill path. See FERMI_TWOD_A.md for the engine model.

#include "NvCanvas2D.h"

#include <cmath>

extern "C" {
#include "nvtypes.h"
#include "class/cl902d.h"      // FERMI_TWOD_A + NV902D_* methods
#include "class/cla06fsubch.h" // NVA06F_SUBCHANNEL_2D (documentation only)
}

#include "NvCommandBuffer.h"


namespace NvCanvas2D {


// ----- Bitmap format helpers ----------------------------------------------

NvU32 Bitmap::BytesPerPixel() const
{
	switch (format) {
		case Format::A8R8G8B8:
		case Format::X8R8G8B8:
		case Format::A8B8G8R8:
			return 4;
		case Format::R5G6B5:
			return 2;
	}
	return 4;
}

NvU32 Bitmap::FormatValue() const
{
	switch (format) {
		case Format::A8R8G8B8: return NV902D_SET_DST_FORMAT_V_A8R8G8B8;
		case Format::X8R8G8B8: return NV902D_SET_DST_FORMAT_V_X8R8G8B8;
		case Format::A8B8G8R8: return NV902D_SET_DST_FORMAT_V_A8B8G8R8;
		case Format::R5G6B5:   return NV902D_SET_DST_FORMAT_V_R5G6B5;
	}
	return NV902D_SET_DST_FORMAT_V_A8R8G8B8;
}


// ----- file-local helpers -------------------------------------------------

// Pack a Color into the surface's pixel encoding (the value the solid-prim
// rasterizer interprets in the destination format). When `premult` is set the
// RGB channels are scaled by alpha first, as BLEND_PREMULT requires.
static NvU32 PackColor(Color c, Format fmt, bool premult)
{
	NvU32 r = c.r, g = c.g, b = c.b, a = c.a;
	if (premult) {
		r = r * a / 255;
		g = g * a / 255;
		b = b * a / 255;
	}
	switch (fmt) {
		case Format::A8R8G8B8:
			return (a << 24) | (r << 16) | (g << 8) | b;
		case Format::X8R8G8B8:
			return (0xFFu << 24) | (r << 16) | (g << 8) | b;
		case Format::A8B8G8R8:
			return (a << 24) | (b << 16) | (g << 8) | r;
		case Format::R5G6B5:
			return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	}
	return 0;
}

// Split a sub-pixel coordinate into the engine's 32.32 fixed-point halves:
// integer part and 32-bit fraction.
static void SplitFixed(float coord, NvU32 &intPart, NvU32 &fracPart)
{
	float f = std::floor(coord);
	intPart  = (NvU32)(NvS32)f;
	fracPart = (NvU32)((coord - f) * 4294967296.0f);
}

// 32.32 per-dst-pixel step in source-pixel units: src extent / dst extent.
static NvU64 ScaleStep(float srcExtent, float dstExtent)
{
	if (dstExtent == 0.0f) {
		return 1ull << 32;
	}
	return (NvU64)((double)srcExtent / (double)dstExtent * 4294967296.0);
}

// HEIGHT field (bits 6:4) of SET_{DST,SRC}_BLOCK_SIZE from a log2-GOBs value;
// DEPTH stays at ONE_GOB (0).
static NvU32 BlockSizeReg(NvU32 blockHeightGobsLog2)
{
	return blockHeightGobsLog2 << 4;
}

// CompositeOp -> NV902D_SET_OPERATION_V_*.
static NvU32 OperationValue(CompositeOp op)
{
	switch (op) {
		case CompositeOp::SrcCopy:      return NV902D_SET_OPERATION_V_SRCCOPY;
		case CompositeOp::BlendPremult: return NV902D_SET_OPERATION_V_BLEND_PREMULT;
		case CompositeOp::Rop:          return NV902D_SET_OPERATION_V_ROP;
	}
	return NV902D_SET_OPERATION_V_SRCCOPY;
}

// Pack a Point into the solid-prim packed vertex word (X in bits 15:0,
// Y in bits 31:16). Coordinates are truncated to unsigned 16-bit.
static NvU32 PackPoint(Point p)
{
	return ((NvU32)p.x & 0xFFFFu) | (((NvU32)p.y & 0xFFFFu) << 16);
}

// Stream `count` vertices to the packed RENDER_SOLID_PRIM_POINT_X_Y trigger,
// chunked under the per-Push reservation cap. Each write of the method emits
// one vertex into the current primitive.
static void StreamPackedPoints(NvCommandBuffer &cmd, NvU32 subch,
	const Point *pts, NvU32 count)
{
	const NvU32 kPerBatch = NvCommandBuffer::kMaxPushDwords - 1; // minus header
	NvU32 i = 0;
	while (i < count) {
		NvU32 n = count - i;
		if (n > kPerBatch) {
			n = kPerBatch;
		}
		NvPushWriter wr = cmd.Push(n + 1);
		wr.NonIncMethod(subch, NV902D_RENDER_SOLID_PRIM_POINT_X_Y, n);
		for (NvU32 j = 0; j < n; j++) {
			wr.U32(PackPoint(pts[i + j]));
		}
		cmd.EndPush(wr);
		i += n;
	}
}


// ----- Canvas -------------------------------------------------------------

Canvas::Canvas(NvCommandBuffer &cmdBuf, const Bitmap &dstBmp, NvU32 twodSubch)
	:
	fCmdBuf(&cmdBuf),
	fDstBmp(dstBmp),
	fSubch(twodSubch)
{
}

void Canvas::EnsureBlendConstants()
{
	if (fBlendBound) {
		return;
	}
	fBlendBound = true;

	// Saturated blend constants so BLEND_PREMULT uses the per-pixel source
	// alpha unmodified (the power-on defaults collapse the blend). Emitted
	// once, the first time a blended draw needs them. See NvRm2dBlitDemo
	// TwodSetupBlendConstants.
	NvPushWriter wr = fCmdBuf->Push(4);
	wr.IncMethod(fSubch, NV902D_SET_BETA1, 1);
	wr.U32(0x7F800000);
	wr.IncMethod(fSubch, NV902D_SET_BETA4, 1);
	wr.U32(0xFFFFFFFFu);
	fCmdBuf->EndPush(wr);
}

void Canvas::BindDst()
{
	if (fDstBound) {
		return;
	}
	fDstBound = true;

	NvPushWriter wr = fCmdBuf->Push(16);
	wr.IncMethod(fSubch, NV902D_SET_DST_FORMAT, 1);
	wr.U32(fDstBmp.FormatValue());

	if (fDstBmp.layout == Layout::BlockLinear) {
		wr.IncMethod(fSubch, NV902D_SET_DST_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR);
		// BLOCK_SIZE + DEPTH + LAYER are contiguous (0x208/0x20c/0x210).
		wr.IncMethod(fSubch, NV902D_SET_DST_BLOCK_SIZE, 3);
		wr.U32(BlockSizeReg(fDstBmp.blockHeightGobsLog2));
		wr.U32(1); // SET_DST_DEPTH
		wr.U32(0); // SET_DST_LAYER
	} else {
		wr.IncMethod(fSubch, NV902D_SET_DST_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH);
		wr.IncMethod(fSubch, NV902D_SET_DST_PITCH, 1);
		wr.U32(fDstBmp.stride);
	}

	// SET_DST_WIDTH + HEIGHT + OFFSET_UPPER + OFFSET_LOWER (contiguous).
	wr.IncMethod(fSubch, NV902D_SET_DST_WIDTH, 4);
	wr.U32(fDstBmp.width);
	wr.U32(fDstBmp.height);
	wr.U32((NvU32)(fDstBmp.gpuAddr >> 32));
	wr.U32((NvU32)(fDstBmp.gpuAddr & 0xFFFFFFFFu));
	fCmdBuf->EndPush(wr);
}

void Canvas::BindSrc(const Bitmap &srcBmp)
{
	NvPushWriter wr = fCmdBuf->Push(16);
	wr.IncMethod(fSubch, NV902D_SET_SRC_FORMAT, 1);
	wr.U32(srcBmp.FormatValue());

	if (srcBmp.layout == Layout::BlockLinear) {
		wr.IncMethod(fSubch, NV902D_SET_SRC_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_SRC_MEMORY_LAYOUT_V_BLOCKLINEAR);
		// BLOCK_SIZE + DEPTH are contiguous (0x238/0x23c); src has no LAYER.
		wr.IncMethod(fSubch, NV902D_SET_SRC_BLOCK_SIZE, 2);
		wr.U32(BlockSizeReg(srcBmp.blockHeightGobsLog2));
		wr.U32(1); // SET_SRC_DEPTH
	} else {
		wr.IncMethod(fSubch, NV902D_SET_SRC_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_SRC_MEMORY_LAYOUT_V_PITCH);
		wr.IncMethod(fSubch, NV902D_SET_SRC_PITCH, 1);
		wr.U32(srcBmp.stride);
	}

	wr.IncMethod(fSubch, NV902D_SET_SRC_WIDTH, 4);
	wr.U32(srcBmp.width);
	wr.U32(srcBmp.height);
	wr.U32((NvU32)(srcBmp.gpuAddr >> 32));
	wr.U32((NvU32)(srcBmp.gpuAddr & 0xFFFFFFFFu));
	fCmdBuf->EndPush(wr);
}

void Canvas::SetRop(NvU8 rop3)
{
	NvPushWriter wr = fCmdBuf->Push(2);
	wr.IncMethod(fSubch, NV902D_SET_ROP, 1);
	wr.U32(rop3);
	fCmdBuf->EndPush(wr);
}

void Canvas::SetClipRect(IntRect rect)
{
	NvPushWriter wr = fCmdBuf->Push(7);
	// X0 / Y0 / WIDTH / HEIGHT are contiguous (0x280..0x28c).
	wr.IncMethod(fSubch, NV902D_SET_CLIP_X0, 4);
	wr.U32((NvU32)rect.left);
	wr.U32((NvU32)rect.top);
	wr.U32((NvU32)(rect.right - rect.left));
	wr.U32((NvU32)(rect.bottom - rect.top));
	wr.IncMethod(fSubch, NV902D_SET_CLIP_ENABLE, 1);
	wr.U32(NV902D_SET_CLIP_ENABLE_V_TRUE);
	fCmdBuf->EndPush(wr);
}

void Canvas::ClearClip()
{
	NvPushWriter wr = fCmdBuf->Push(2);
	wr.IncMethod(fSubch, NV902D_SET_CLIP_ENABLE, 1);
	wr.U32(NV902D_SET_CLIP_ENABLE_V_FALSE);
	fCmdBuf->EndPush(wr);
}

void Canvas::SetColorKey(Color key)
{
	NvPushWriter wr = fCmdBuf->Push(4);
	// FORMAT / KEY / ENABLE are contiguous (0x294..0x29c).
	wr.IncMethod(fSubch, NV902D_SET_COLOR_KEY_FORMAT, 3);
	wr.U32(NV902D_SET_COLOR_KEY_FORMAT_V_A8R8G8B8);
	wr.U32(PackColor(key, Format::A8R8G8B8, /*premult=*/false));
	wr.U32(NV902D_SET_COLOR_KEY_ENABLE_V_TRUE);
	fCmdBuf->EndPush(wr);
}

void Canvas::ClearColorKey()
{
	NvPushWriter wr = fCmdBuf->Push(2);
	wr.IncMethod(fSubch, NV902D_SET_COLOR_KEY_ENABLE, 1);
	wr.U32(NV902D_SET_COLOR_KEY_ENABLE_V_FALSE);
	fCmdBuf->EndPush(wr);
}

void Canvas::BeginSolidPrim(Color color, CompositeOp op, NvU32 primMode)
{
	BindDst();
	if (op == CompositeOp::BlendPremult) {
		EnsureBlendConstants();
	}

	NvPushWriter wr = fCmdBuf->Push(8);
	wr.IncMethod(fSubch, NV902D_SET_OPERATION, 1);
	wr.U32(OperationValue(op));

	wr.IncMethod(fSubch, NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT, 1);
	wr.U32(fDstBmp.FormatValue());

	wr.IncMethod(fSubch, NV902D_SET_RENDER_SOLID_PRIM_COLOR, 1);
	wr.U32(PackColor(color, fDstBmp.format, /*premult=*/op == CompositeOp::BlendPremult));

	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_MODE, 1);
	wr.U32(primMode);
	fCmdBuf->EndPush(wr);
}

void Canvas::FillRect(IntRect rect, Color color, CompositeOp op)
{
	BeginSolidPrim(color, op, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS);

	// Two vertices (top-left, bottom-right) define the rect; writing each
	// POINT's Y triggers it, and the second completes the RECTS primitive.
	// SET_X(j)/Y(j) are contiguous (0x600+j*8, 0x604+j*8). Indexed 32-bit
	// slots keep the full coordinate range for large canvases.
	NvPushWriter wr = fCmdBuf->Push(6);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 2);
	wr.U32((NvU32)rect.left);
	wr.U32((NvU32)rect.top);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 2);
	wr.U32((NvU32)rect.right);
	wr.U32((NvU32)rect.bottom);
	fCmdBuf->EndPush(wr);
}

void Canvas::StrokeLine(Point a, Point b, Color color, CompositeOp op)
{
	BeginSolidPrim(color, op, NV902D_RENDER_SOLID_PRIM_MODE_V_LINES);

	NvPushWriter wr = fCmdBuf->Push(6);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 2);
	wr.U32((NvU32)a.x);
	wr.U32((NvU32)a.y);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 2);
	wr.U32((NvU32)b.x);
	wr.U32((NvU32)b.y);
	fCmdBuf->EndPush(wr);
}

void Canvas::FillTriangle(Point a, Point b, Point c, Color color, CompositeOp op)
{
	BeginSolidPrim(color, op, NV902D_RENDER_SOLID_PRIM_MODE_V_TRIANGLES);

	NvPushWriter wr = fCmdBuf->Push(9);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 2);
	wr.U32((NvU32)a.x);
	wr.U32((NvU32)a.y);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 2);
	wr.U32((NvU32)b.x);
	wr.U32((NvU32)b.y);
	wr.IncMethod(fSubch, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(2), 2);
	wr.U32((NvU32)c.x);
	wr.U32((NvU32)c.y);
	fCmdBuf->EndPush(wr);
}

void Canvas::StrokePolyline(const Point *pts, NvU32 count, Color color,
	bool closed, CompositeOp op)
{
	if (count == 0) {
		return;
	}
	BeginSolidPrim(color, op, NV902D_RENDER_SOLID_PRIM_MODE_V_POLYLINE);
	StreamPackedPoints(*fCmdBuf, fSubch, pts, count);
	if (closed) {
		StreamPackedPoints(*fCmdBuf, fSubch, &pts[0], 1);
	}
}

void Canvas::DrawPoints(const Point *pts, NvU32 count, Color color, CompositeOp op)
{
	if (count == 0) {
		return;
	}
	BeginSolidPrim(color, op, NV902D_RENDER_SOLID_PRIM_MODE_V_POINTS);
	StreamPackedPoints(*fCmdBuf, fSubch, pts, count);
}

void Canvas::DrawBitmap(const Bitmap &srcBmp, Rect dstRect, Rect srcRect,
	BitmapFilter filter, CompositeOp op)
{
	BindDst();
	BindSrc(srcBmp);
	if (op == CompositeOp::BlendPremult) {
		EnsureBlendConstants();
	}

	const NvU32 dstX = (NvU32)(NvS32)std::lround(dstRect.left);
	const NvU32 dstY = (NvU32)(NvS32)std::lround(dstRect.top);
	const NvU32 dstW = (NvU32)(NvS32)std::lround(dstRect.right - dstRect.left);
	const NvU32 dstH = (NvU32)(NvS32)std::lround(dstRect.bottom - dstRect.top);

	const NvU64 duDx = ScaleStep(srcRect.right - srcRect.left, dstRect.right - dstRect.left);
	const NvU64 dvDy = ScaleStep(srcRect.bottom - srcRect.top, dstRect.bottom - dstRect.top);

	NvU32 srcXInt, srcXFrac, srcYInt, srcYFrac;
	SplitFixed(srcRect.left, srcXInt, srcXFrac);
	SplitFixed(srcRect.top,  srcYInt, srcYFrac);

	NvPushWriter wr = fCmdBuf->Push(17);
	wr.IncMethod(fSubch, NV902D_SET_OPERATION, 1);
	wr.U32(OperationValue(op));

	// SAMPLE_MODE: ORIGIN at bit 0, FILTER at bit 4. DRF_DEF can't be used
	// here (GCC parses the `902D` class token as a UDL pp-number before the
	// macro fires), so build the value from the resolved constants.
	const NvU32 sampleMode =
		(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN_CORNER << 0) |
		((filter == BitmapFilter::Bilinear
			? NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_BILINEAR
			: NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_POINT) << 4);
	wr.IncMethod(fSubch, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 1);
	wr.U32(sampleMode);

	// 12 contiguous methods starting at DST_X0; writing the final word
	// (SRC_Y0_INT) triggers the blit.
	wr.IncMethod(fSubch, NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 12);
	wr.U32(dstX);
	wr.U32(dstY);
	wr.U32(dstW);
	wr.U32(dstH);
	wr.U32((NvU32)(duDx & 0xFFFFFFFFu)); // DU_DX_FRAC
	wr.U32((NvU32)(duDx >> 32));         // DU_DX_INT
	wr.U32((NvU32)(dvDy & 0xFFFFFFFFu)); // DV_DY_FRAC
	wr.U32((NvU32)(dvDy >> 32));         // DV_DY_INT
	wr.U32(srcXFrac);                    // SRC_X0_FRAC
	wr.U32(srcXInt);                     // SRC_X0_INT
	wr.U32(srcYFrac);                    // SRC_Y0_FRAC
	wr.U32(srcYInt);                     // SRC_Y0_INT
	fCmdBuf->EndPush(wr);
}


}
