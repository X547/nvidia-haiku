// NvRmCanvasGuiDemo
//
// Displays NvCanvas2D (FERMI_TWOD_A) output in a live Haiku window with a
// zero-copy present path.
//
// FERMI_TWOD_A cannot render into a sysmem target (it raises a GR class
// error), so each frame uses two GPU surfaces:
//   - a VRAM scratch surface the 2D engine composes the scene into, and
//   - a GART (sysmem) present buffer that is CPU-mapped as a Haiku `area`.
// A copy-engine blit moves scratch -> present buffer (a GPU-side copy, not a
// CPU memcpy). The present buffer's area is handed to BBitmap via its
// (area_id, offset, ...) constructor; the app_server clones that area
// (BitmapManager::CloneFromClient), so it reads the very same physical pages
// the CE wrote — no memcpy anywhere on the present path.
//
// Both surfaces are sized to the view (same pitch) and reallocated on every
// resize. Each frame the window thread records the scene + CE blit into one
// submission (ordered on the GPU by a ROP-cache flush and a wait-for-idle
// barrier between the two phases), waits once for the trailing release (GART
// is cached + snooped, so the result is CPU-coherent once it lands), then
// DrawBitmap()s it.
//
// A8R8G8B8 in NVIDIA's encoding is byte order B,G,R,A in memory, which is
// exactly Haiku's B_RGB32 — so the surface format matches with no swizzle.

#include <Application.h>
#include <Window.h>
#include <View.h>
#include <Bitmap.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <math.h>

#include <system_error>
#include <vector>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
#include "class/cl902d.h"              // FERMI_TWOD_A
#include "class/cla06fsubch.h"         // NVA06F_SUBCHANNEL_2D, NVA06F_SUBCHANNEL_COPY_ENGINE
#include "class/clc5b5.h"              // TURING_DMA_COPY_A
#include "class/clc6b5.h"              // AMPERE_DMA_COPY_A
#include "class/clc7b5.h"              // AMPERE_DMA_COPY_B
#include "class/clc8b5.h"              // HOPPER_DMA_COPY_A
#include "class/clc9b5.h"              // BLACKWELL_DMA_COPY_A
#include "class/clcab5.h"              // BLACKWELL_DMA_COPY_B
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
}

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvOsEvent.h"
#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"
#include "NvSemSurf.h"
#include "NvCanvas2D.h"


static const NvU32 kTwodSubch = NVA06F_SUBCHANNEL_2D;
static const NvU32 kCeSubch   = NVA06F_SUBCHANNEL_COPY_ENGINE;
static const NvU32 kBpp = 4; // A8R8G8B8 / B_RGB32

// Render-target pitch alignment. The 2D engine renders into a VRAM scratch
// surface at this pitch; the CE then linearly copies pitch*height bytes into
// the GART buffer, which carries the same pitch (handed to BBitmap as
// bytesPerRow). 256 keeps the pitch comfortably within any engine alignment
// requirement for arbitrary window widths.
static const NvU32 kPitchAlign = 256;


// ----- shared GPU plumbing (mirrors NvRmCanvas2DDemo) ----------------------

static void RequireClass(NvRmDevice &rmDev, NvU32 wanted, const char *name)
{
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	rmDev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 i = 0; i < params.numClasses; i++) {
		if (params.classList[i] == wanted) {
			return;
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(),
		std::string("GPU does not advertise ") + name);
}

static NvU32 PickCopyClass(NvRmDevice &rmDev)
{
	static const NvU32 kCandidates[] = {
		BLACKWELL_DMA_COPY_B,
		BLACKWELL_DMA_COPY_A,
		HOPPER_DMA_COPY_A,
		AMPERE_DMA_COPY_B,
		AMPERE_DMA_COPY_A,
		TURING_DMA_COPY_A,
	};
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	rmDev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 cand : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == cand) {
				return cand;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no supported DMA copy class");
}

static void EmitDoneRelease(NvCommandBuffer &cmdBuf, NvSemSurf &semSurf, NvU64 slot, NvU64 wSeq)
{
	semSurf.RecordSubmission(slot, wSeq);
	semSurf.WriteRelease(cmdBuf, slot, wSeq, /*waitForIdle=*/true);
	WriteNonStallInterrupt(cmdBuf);
}

// A wait-for-idle semaphore release with no CPU wakeup interrupt. Stalls the
// pushbuffer until all prior channel work (the 2D render) has retired, so the
// CE copy that follows in the same submission is ordered after it. The CPU
// never waits on this value — it only serializes the GPU.
static void EmitWfiBarrier(NvCommandBuffer &cmdBuf, NvSemSurf &semSurf, NvU64 slot, NvU64 wSeq)
{
	semSurf.RecordSubmission(slot, wSeq);
	semSurf.WriteRelease(cmdBuf, slot, wSeq, /*waitForIdle=*/true);
}

static void SubmitAndWait(NvChannel &channel, NvCommandBuffer &cmdBuf,
	NvSemSurf &semSurf, NvOsEvent &osEvent, NvU64 slot, NvU64 wSeq)
{
	semSurf.RegisterWaiter(slot, wSeq, 0, (NvU64)osEvent.Fd());

	cmdBuf.SubmitTo(channel);
	while (semSurf.ReadValue(slot) < wSeq) {
		channel.CheckError();
		struct pollfd pfd { .fd = osEvent.Fd(), .events = POLLIN | POLLPRI };
		int pr = poll(&pfd, 1, 5000);
		if (pr < 0) {
			CheckErrno(pr);
		}
	}
}


// ----- the animated scene --------------------------------------------------

// Ping-pong a value in [0, range] from a monotonically increasing phase.
static NvS32 PingPong(NvU32 phase, NvS32 range)
{
	if (range <= 0) {
		return 0;
	}
	NvU32 period = (NvU32)(2 * range);
	NvU32 p = phase % period;
	return (NvS32)p <= range ? (NvS32)p : (NvS32)(period - p);
}

// Compose a size-dependent, animated scene that exercises most Canvas
// methods. `tick` advances once per pulse.
static void DrawScene(NvCanvas2D::Canvas &canvas, NvU32 w, NvU32 h, NvU32 tick)
{
	using namespace NvCanvas2D;

	// Opaque background clear.
	canvas.FillRect({0, 0, (NvS32)w, (NvS32)h}, Color{0x10, 0x14, 0x20, 0xFF});

	// Grid lines, spaced with the view so they reflow on resize.
	const Color grid {0x28, 0x34, 0x50, 0xFF};
	for (NvU32 x = 0; x < w; x += 64) {
		canvas.StrokeLine({(NvS32)x, 0}, {(NvS32)x, (NvS32)h - 1}, grid);
	}
	for (NvU32 y = 0; y < h; y += 64) {
		canvas.StrokeLine({0, (NvS32)y}, {(NvS32)w - 1, (NvS32)y}, grid);
	}

	// A sine wave polyline spanning the full width, phase-shifted by tick.
	if (w >= 4) {
		std::vector<Point> wave;
		const float amp = h * 0.18f;
		const float midY = h * 0.5f;
		for (NvU32 x = 0; x < w; x += 4) {
			float ph = (float)x / 48.0f - (float)tick * 0.15f;
			wave.push_back({(NvS32)x, (NvS32)(midY + amp * sinf(ph))});
		}
		canvas.StrokePolyline(wave.data(), (NvU32)wave.size(),
			Color{0x40, 0xC0, 0xFF, 0xFF});
	}

	// Bouncing opaque square.
	const NvS32 box = 120;
	NvS32 bx = PingPong(tick * 7, (NvS32)w - box);
	NvS32 by = PingPong(tick * 5, (NvS32)h - box);
	canvas.FillRect({bx, by, bx + box, by + box}, Color{0xFF, 0x80, 0x20, 0xFF});

	// Rotating filled triangle about the center.
	{
		const float cx = w * 0.5f, cy = h * 0.5f;
		const float rad = (w < h ? w : h) * 0.22f;
		const float a = tick * 0.05f;
		Point t[3];
		for (int i = 0; i < 3; i++) {
			float ang = a + i * (2.0f * (float)M_PI / 3.0f);
			t[i] = {(NvS32)(cx + rad * cosf(ang)), (NvS32)(cy + rad * sinf(ang))};
		}
		canvas.FillTriangle(t[0], t[1], t[2], Color{0xE0, 0x40, 0x80, 0xFF});
	}

	// Translucent cyan panel composited over everything (BLEND_PREMULT).
	NvS32 pw = (NvS32)w / 4, ph2 = (NvS32)h / 4;
	canvas.FillRect({(NvS32)w - pw - 32, 32, (NvS32)w - 32, 32 + ph2},
		Color{0x20, 0xC0, 0xE0, 0x90}, CompositeOp::BlendPremult);
}


// ----- GPU renderer: owns the channel, 2D object, and the GART draw buffer --

class NvGpuRenderer {
private:
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvChannel fChannel;
	NvCommandBuffer fCmdBuf;
	NvSemSurf fSemSurf;
	NvOsEvent fOsEvent;
	NvU32 fCopyClass {};
	NvRmObject fTwodObj;
	NvRmObject fCeObj;
	NvRmObject fErrEvent;

	NvGpuMemory fScratch; // VRAM: the 2D engine render target
	NvGpuMemory fGart;    // sysmem: CPU-mapped, wrapped by BBitmap (present buffer)
	NvU32 fWidth {};
	NvU32 fHeight {};
	NvU32 fPitch {};      // bytes per row, shared by scratch + gart
	NvU64 fCopyBytes {};
	NvU64 fSeq {};

public:
	NvGpuRenderer()
		:
		fRmDev(fRm, 0),
		fChannel(fRmDev),
		fCmdBuf(fChannel),
		fSemSurf(fRmDev, fChannel.VaSpace(), 1),
		fOsEvent(fRm)
	{
		RequireClass(fRmDev, FERMI_TWOD_A, "FERMI_TWOD_A");
		fCopyClass = PickCopyClass(fRmDev);
		fTwodObj = fChannel.Channel().Alloc(FERMI_TWOD_A, nullptr, 0);
		fCeObj   = fChannel.Channel().Alloc(fCopyClass,   nullptr, 0);
		fSemSurf.BindChannel(fChannel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});
		fErrEvent = AllocSubdeviceOsEvent(
			fRmDev, fOsEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);
	}

	NvU32 Width()  const { return fWidth; }
	NvU32 Height() const { return fHeight; }
	NvU32 Pitch()  const { return fPitch; }

	// (Re)allocate the render target (VRAM scratch) and the present buffer
	// (CPU-mapped GART) for a `w`x`h` A8R8G8B8 surface, both at the same
	// pitch. Returns the area_id backing the GART buffer's CPU mapping, for
	// BBitmap. The 2D engine cannot render into sysmem, so it draws into the
	// VRAM scratch and a CE blit moves the result into the GART buffer.
	area_id ResizeBuffer(NvU32 w, NvU32 h)
	{
		fWidth  = w;
		fHeight = h;
		fPitch  = ((w * kBpp) + (kPitchAlign - 1)) & ~(kPitchAlign - 1);
		fCopyBytes = (NvU64)fPitch * h;

		fScratch.AllocLocal(fRmDev, fChannel.VaSpace(), fCopyBytes, 0x1000);

		NvU64 gartSize = (fCopyBytes + 0xFFF) & ~(NvU64)0xFFF; // page-rounded for the area
		fGart.AllocSystem(fRmDev, fChannel.VaSpace(), gartSize, 0x1000, /*mapCpu=*/true);
		return fGart.CpuMap().Area();
	}

	// Render one frame in a single submission: (1) the 2D engine composes the
	// scene into VRAM scratch, then (2) a CE blit copies it into the GART
	// present buffer. The two phases are ordered on the GPU — a ROP-cache
	// flush makes the 2D writes visible and a wait-for-idle barrier stalls the
	// pushbuffer until the 2D engine is idle — so the CE never reads the
	// scratch early and no CPU round-trip is needed between them. Only the
	// final release is waited on (we must know the present buffer is filled
	// before DrawBitmap).
	void RenderFrame(NvU32 tick)
	{
		using namespace NvCanvas2D;

		// Phase 1: 2D draws into the VRAM scratch. SET_OBJECT persists on the
		// channel, but the command buffer is recycled each submit, so re-bind
		// cheaply every frame.
		WriteSetObject(fCmdBuf, kTwodSubch, FERMI_TWOD_A);

		Bitmap dst;
		dst.gpuAddr = fScratch.GpuAddr();
		dst.width   = fWidth;
		dst.height  = fHeight;
		dst.stride  = fPitch;
		dst.format  = Format::A8R8G8B8;
		dst.layout  = Layout::Pitch;

		Canvas canvas(fCmdBuf, dst, kTwodSubch);
		DrawScene(canvas, fWidth, fHeight, tick);

		// Flush the 2D ROP mini-cache so the scratch writes reach L2 before
		// the CE reads them.
		{
			NvPushWriter wr = fCmdBuf.Push(2);
			wr.IncMethod(kTwodSubch, NV902D_FLUSH_AND_INVALIDATE_ROP_MINI_CACHE, 1);
			wr.U32(0);
			fCmdBuf.EndPush(wr);
		}
		// Host barrier: stall until the 2D render retires, ordering the CE
		// copy after it (not waited on by the CPU).
		EmitWfiBarrier(fCmdBuf, fSemSurf, 0, ++fSeq);

		// Phase 2: CE copies scratch -> GART present buffer (same pitch, so a
		// single linear copy reproduces every row).
		WriteSetObject(fCmdBuf, kCeSubch, fCopyClass);
		WriteCopyDma(fCmdBuf, kCeSubch, fScratch.GpuAddr(), fGart.GpuAddr(), fCopyBytes);
		EmitDoneRelease(fCmdBuf, fSemSurf, 0, ++fSeq);

		// One wait, for the whole frame.
		SubmitAndWait(fChannel, fCmdBuf, fSemSurf, fOsEvent, 0, fSeq);
	}
};


// ----- Haiku view / window / app -------------------------------------------

class NvCanvasView : public BView {
private:
	NvGpuRenderer &fRenderer;
	BBitmap *fBitmap {};
	uint32 fTick {};

	void Realloc()
	{
		BRect b = Bounds();
		NvU32 w = (NvU32)(b.IntegerWidth() + 1);
		NvU32 h = (NvU32)(b.IntegerHeight() + 1);
		if (w == 0 || h == 0) {
			return;
		}
		area_id area = fRenderer.ResizeBuffer(w, h);
		delete fBitmap;
		// Zero-copy present: BBitmap wraps the GART area directly (offset 0).
		// The server clones this area, sharing pages with the buffer the CE
		// blits into — no CPU memcpy. bytesPerRow matches the GART pitch.
		fBitmap = new BBitmap(area, 0, BRect(0, 0, w - 1, h - 1),
			B_BITMAP_IS_AREA, B_RGB32, (int32)fRenderer.Pitch());
	}

public:
	NvCanvasView(BRect frame, NvGpuRenderer &renderer)
		:
		BView(frame, "nvcanvas", B_FOLLOW_ALL,
			B_WILL_DRAW | B_PULSE_NEEDED | B_FRAME_EVENTS),
		fRenderer(renderer)
	{
	}

	~NvCanvasView() override
	{
		delete fBitmap;
	}

	void AttachedToWindow() override
	{
		Realloc();
	}

	void FrameResized(float, float) override
	{
		Realloc();
		Invalidate();
	}

	void Pulse() override
	{
		fTick++;
		Invalidate();
	}

	void Draw(BRect) override
	{
		if (fBitmap == nullptr) {
			return;
		}
		try {
			fRenderer.RenderFrame(fTick);
		} catch (const std::exception &ex) {
			fprintf(stderr, "[!] render: %s\n", ex.what());
			return;
		}
		DrawBitmap(fBitmap, B_ORIGIN);
	}
};


class NvCanvasWindow : public BWindow {
public:
	NvCanvasWindow(NvGpuRenderer &renderer)
		:
		BWindow(BRect(100, 100, 900, 700), "NvCanvas2D — GPU 2D (zero-copy)",
			B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	{
		NvCanvasView *view = new NvCanvasView(Bounds(), renderer);
		AddChild(view);
		SetPulseRate(1000000 / 60); // 60 fps animation
	}
};


class NvCanvasApp : public BApplication {
private:
	NvGpuRenderer &fRenderer;

public:
	NvCanvasApp(NvGpuRenderer &renderer)
		:
		BApplication("application/x-vnd.nvidia-NvRmCanvasGuiDemo"),
		fRenderer(renderer)
	{
	}

	void ReadyToRun() override
	{
		(new NvCanvasWindow(fRenderer))->Show();
	}
};


int main()
{
	try {
		// GPU init happens up front in the main thread so failures (no device,
		// missing FERMI_TWOD_A) surface cleanly before the window opens.
		NvGpuRenderer renderer;
		NvCanvasApp app(renderer);
		app.Run();
		return 0;
	} catch (const std::exception &ex) {
		fprintf(stderr, "[!] NvRmCanvasGuiDemo: %s\n", ex.what());
		return 1;
	}
}
