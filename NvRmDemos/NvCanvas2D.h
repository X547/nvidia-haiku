#pragma once

extern "C" {
#include "nvtypes.h"
}


class NvCommandBuffer;


// A thin, record-only 2D drawing surface on top of FERMI_TWOD_A. A Canvas
// writes TWOD methods into an NvCommandBuffer the caller owns; it never
// submits, waits, or allocates GPU objects. The caller is responsible for:
//   - allocating a FERMI_TWOD_A object on the channel,
//   - binding it to `twodSubch` via WriteSetObject() *before* the first draw,
//   - submitting the command buffer and waiting for completion.
// This mirrors how NvRm2dBlitDemo drives the engine, just hoisted into a
// reusable shape. See FERMI_TWOD_A.md for the engine model.
namespace NvCanvas2D {


// Integer destination rectangle (pixel grid) used by FillRect.
struct IntRect {
	NvS32 left;
	NvS32 top;
	NvS32 right;
	NvS32 bottom;
};

// Sub-pixel rectangle used by DrawBitmap for src/dst so fractional scale
// and sampling offsets are expressible.
struct Rect {
	float left;
	float top;
	float right;
	float bottom;
};

// Integer vertex for the solid-prim geometry methods (lines / triangles /
// points). 32-bit signed for StrokeLine / FillTriangle; StrokePolyline and
// DrawPoints pack to unsigned 16-bit (see those methods).
struct Point {
	NvS32 x;
	NvS32 y;
};


struct Color {
	NvU8 r;
	NvU8 g;
	NvU8 b;
	NvU8 a;
};


// Composition mode shared by every fill / blit / geometry draw. Maps to
// NV902D_SET_OPERATION_V_*.
enum class CompositeOp {
	SrcCopy,       // dst = src
	BlendPremult,  // Porter-Duff "over"; source/color treated as premultiplied
	Rop,           // ROP3 of source & destination — set the code via SetRop()
};


// Surface pixel format. Values map to the shared NV902D_SET_{DST,SRC}_FORMAT_V_*
// / SET_RENDER_SOLID_PRIM_COLOR_FORMAT_V_* enum (the same byte encodes the
// format for dst, src, and solid-prim color).
enum class Format {
	A8R8G8B8,
	X8R8G8B8,
	A8B8G8R8,
	R5G6B5,
};

// Memory tiling of a surface. BlockLinear additionally consults
// Bitmap::blockHeightGobsLog2.
enum class Layout {
	Pitch,
	BlockLinear,
};

enum class BitmapFilter {
	Point,
	Bilinear,
};


// Describes a GPU surface the 2D engine reads or writes. Holds no ownership:
// the backing NvGpuMemory (and its lifetime) belong to the caller; Bitmap is
// just the geometry/format the engine needs to address it.
class Bitmap {
public:
	NvU64 gpuAddr {};
	NvU32 width   {};
	NvU32 height  {};
	NvU32 stride  {};                 // bytes per row; required for Pitch layout
	Format format {Format::A8R8G8B8};
	Layout layout {Layout::Pitch};
	// log2 of the GOB block height (0 = 1 GOB, 1 = 2 GOBs, ... 5 = 32 GOBs).
	// Only consulted when layout == BlockLinear; written into the HEIGHT field
	// (bits 6:4) of SET_{DST,SRC}_BLOCK_SIZE.
	NvU32 blockHeightGobsLog2 {0};

	// Bytes per pixel implied by `format`.
	NvU32 BytesPerPixel() const;
	// The NV902D_SET_*_FORMAT_V_* register value for `format`.
	NvU32 FormatValue() const;
};


class Canvas {
private:
	NvCommandBuffer *fCmdBuf {};
	Bitmap fDstBmp {};
	NvU32 fSubch {};
	bool fDstBound {};
	bool fBlendBound {};

	// Emit the destination surface state once (format / layout / dimensions /
	// address). Lazy so a freshly-constructed Canvas costs nothing until the
	// first draw, and so it lands after the caller's WriteSetObject().
	void BindDst();
	// Emit source surface state for a DrawBitmap.
	void BindSrc(const Bitmap &srcBmp);
	// Emit the saturated blend constants (SET_BETA1 / SET_BETA4) once, the
	// first time a BlendPremult draw needs them.
	void EnsureBlendConstants();
	// Shared solid-prim preamble: operation, color format, packed color, and
	// primitive mode (NV902D_RENDER_SOLID_PRIM_MODE_V_*). Calls BindDst().
	void BeginSolidPrim(Color color, CompositeOp op, NvU32 primMode);

public:
	// Records nothing on its own — all engine state (destination surface,
	// blend constants) is emitted lazily on the first draw that needs it, so
	// it lands after the caller's WriteSetObject(). Does NOT emit SET_OBJECT —
	// the caller must bind FERMI_TWOD_A to `twodSubch` first (see class comment).
	Canvas(NvCommandBuffer &cmdBuf, const Bitmap &dstBmp, NvU32 twodSubch);

	// ----- composition state (gates subsequent draws) ---------------------

	// 8-bit Windows ROP3 code used by CompositeOp::Rop draws (e.g. 0xCC
	// SRCCOPY, 0x66 SRCINVERT, 0x55 DSTINVERT). Only src/dst ROPs are
	// meaningful here — there is no pattern surface, so pattern-term codes
	// are undefined. Emits SET_ROP immediately; stays in effect until changed.
	void SetRop(NvU8 rop3);

	// Scissor rectangle limiting all destination writes until ClearClip().
	void SetClipRect(IntRect rect);
	void ClearClip();

	// Chroma-key transparency for DrawBitmap: source pixels equal to `key`
	// (compared as A8R8G8B8) are skipped. Stays enabled until ClearColorKey().
	void SetColorKey(Color key);
	void ClearColorKey();

	// ----- draws ----------------------------------------------------------

	// Solid-color rectangle via the solid-prim RECTS rasterizer. CompositeOp
	// selects overwrite (SrcCopy), alpha composite (BlendPremult — the color
	// is premultiplied by its alpha), or ROP3 (Rop, using SetRop()).
	void FillRect(IntRect rect, Color color, CompositeOp op = CompositeOp::SrcCopy);

	// 1px line between two points (LINES mode). Width is fixed by the engine;
	// thicker strokes must be built from filled rects/triangles.
	void StrokeLine(Point a, Point b, Color color, CompositeOp op = CompositeOp::SrcCopy);

	// Connected 1px polyline through `count` points (POLYLINE mode). When
	// `closed` is set the last point is joined back to the first. Vertices are
	// streamed packed as unsigned 16-bit, so coordinates must be in [0, 65535].
	void StrokePolyline(const Point *pts, NvU32 count, Color color,
		bool closed = false, CompositeOp op = CompositeOp::SrcCopy);

	// Flat-shaded filled triangle (TRIANGLES mode).
	void FillTriangle(Point a, Point b, Point c, Color color,
		CompositeOp op = CompositeOp::SrcCopy);

	// `count` single-pixel points (POINTS mode). Vertices are streamed packed
	// as unsigned 16-bit, so coordinates must be in [0, 65535].
	void DrawPoints(const Point *pts, NvU32 count, Color color,
		CompositeOp op = CompositeOp::SrcCopy);

	// Scaled/filtered surface blit via SetPixelsFromMemory. `srcRect` /
	// `dstRect` are in source / destination pixels (sub-pixel allowed); the
	// per-dst-pixel step is derived from their size ratio. CompositeOp selects
	// straight copy, premultiplied "over", or ROP3.
	void DrawBitmap(const Bitmap &srcBmp, Rect dstRect, Rect srcRect,
		BitmapFilter filter, CompositeOp op = CompositeOp::SrcCopy);
};


}
