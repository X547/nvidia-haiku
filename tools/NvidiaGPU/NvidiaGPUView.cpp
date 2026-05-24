/*
 * NvidiaGPUView.cpp - Main view displaying GPU information
 */

#include "NvidiaGPUView.h"
#include "GPUProgressBar.h"
#include "Common.h"

#include <Alert.h>
#include <Application.h>
#include <Dragger.h>
#include <Roster.h>
#include <Shelf.h>
#include <Window.h>

#include <stdio.h>
#include <string.h>


NvidiaGPUView::NvidiaGPUView(BRect frame)
	: BView(frame, "NvidiaGPUView", B_FOLLOW_NONE,
	        B_WILL_DRAW | B_PULSE_NEEDED | B_FRAME_EVENTS),
	  fGpuLoadBar(NULL),
	  fMemLoadBar(NULL),
	  fPopupMenu(NULL),
	  fClonedArea(-1),
	  fStats(NULL),
	  fGpuTemp(0),
	  fGpuClockMHz(0),
	  fMemClockMHz(0),
	  fPowerMw(0),
	  fValidMask(0),
	  fMessageRunner(NULL),
	  fIsReplicant(false)
{
	Init();
}


NvidiaGPUView::NvidiaGPUView(BMessage* archive)
	: BView(archive),
	  fGpuLoadBar(NULL),
	  fMemLoadBar(NULL),
	  fPopupMenu(NULL),
	  fClonedArea(-1),
	  fStats(NULL),
	  fGpuTemp(0),
	  fGpuClockMHz(0),
	  fMemClockMHz(0),
	  fPowerMw(0),
	  fValidMask(0),
	  fMessageRunner(NULL),
	  fIsReplicant(true)
{
	Init();
}


void
NvidiaGPUView::Init()
{
	SetViewColor(B_TRANSPARENT_COLOR);  // Allow alpha background

	// Create progress bars
	rgb_color gpuColor = { GPU_LOAD_COLOR_R, GPU_LOAD_COLOR_G, GPU_LOAD_COLOR_B, 255 };
	rgb_color memColor = { MEM_LOAD_COLOR_R, MEM_LOAD_COLOR_G, MEM_LOAD_COLOR_B, 255 };

	// Center content vertically: content height ~72px, view height 95px, so top margin = 12px
	float topMargin = 12;
	BRect barRect(LABEL_WIDTH + 10, topMargin, LABEL_WIDTH + 10 + PROGRESS_BAR_WIDTH, topMargin + PROGRESS_BAR_HEIGHT);
	fGpuLoadBar = new GPUProgressBar(barRect, "gpu_load", gpuColor);
	AddChild(fGpuLoadBar);

	barRect.OffsetBy(0, PROGRESS_BAR_HEIGHT + 8);
	fMemLoadBar = new GPUProgressBar(barRect, "mem_load", memColor);
	AddChild(fMemLoadBar);

	// Add dragger for replicant support (bottom-right corner)
	BRect bounds = Bounds();
	BRect draggerRect(bounds.right - 7, bounds.bottom - 7, bounds.right, bounds.bottom);
	BDragger* dragger = new BDragger(draggerRect, this, B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
	AddChild(dragger);

	InitMenu();
}


void
NvidiaGPUView::InitMenu()
{
	fPopupMenu = new BPopUpMenu("PopUpMenu", false, false, B_ITEMS_IN_COLUMN);
	fPopupMenu->SetFont(be_plain_font);

	fAboutItem = new BMenuItem("About NvidiaGPU" B_UTF8_ELLIPSIS,
	                           new BMessage(MSG_ABOUT), 0, 0);
	fQuitItem = new BMenuItem("Quit", new BMessage(MSG_QUIT), 0, 0);

	fPopupMenu->AddItem(fAboutItem);
	fPopupMenu->AddSeparatorItem();
	fPopupMenu->AddItem(fQuitItem);
}


NvidiaGPUView::~NvidiaGPUView()
{
	DisconnectFromStats();

	delete fMessageRunner;
	delete fPopupMenu;
}


void
NvidiaGPUView::AttachedToWindow()
{
	BView::AttachedToWindow();

	ConnectToStats();

	// Set menu targets to this view
	BMessenger messenger(this);
	fAboutItem->SetTarget(messenger);
	fQuitItem->SetTarget(messenger);

	// When running as replicant, use message runner for updates
	// (Pulse() may not work reliably in replicant mode)
	if (fIsReplicant) {
		fMessageRunner = new BMessageRunner(messenger,
		                                    new BMessage(MSG_REPLICANT_PULSE),
		                                    500000, -1);
	}

	// Initial update
	Update();
}


void
NvidiaGPUView::DetachedFromWindow()
{
	delete fMessageRunner;
	fMessageRunner = NULL;

	BView::DetachedFromWindow();
}


bool
NvidiaGPUView::ConnectToStats()
{
	if (fStats != NULL)
		return true;

	area_id statsAreaId = find_area(NVIDIA_GPU_STATS_AREA_NAME);
	if (statsAreaId < 0)
		return false;

	fClonedArea = clone_area("nvidia_gpu_stats_clone",
	                         (void**)&fStats,
	                         B_ANY_ADDRESS,
	                         B_READ_AREA,
	                         statsAreaId);

	if (fClonedArea < 0) {
		fStats = NULL;
		return false;
	}

	if (fStats->magic != NVIDIA_GPU_STATS_MAGIC) {
		delete_area(fClonedArea);
		fClonedArea = -1;
		fStats = NULL;
		return false;
	}

	return true;
}


void
NvidiaGPUView::DisconnectFromStats()
{
	if (fClonedArea >= 0) {
		delete_area(fClonedArea);
		fClonedArea = -1;
		fStats = NULL;
	}
}


void
NvidiaGPUView::Update()
{
	// Try to connect if not connected
	if (fStats == NULL)
		ConnectToStats();

	if (fStats != NULL) {
		fValidMask = fStats->validMask;
		fGpuTemp = fStats->gpuTemp;
		fGpuClockMHz = fStats->gpuClockMHz;
		fMemClockMHz = fStats->memClockMHz;
		fPowerMw = fStats->powerMw;

		// Update progress bars
		if (fValidMask & NVIDIA_STATS_VALID_GPU_UTIL)
			fGpuLoadBar->SetValue(fStats->gpuUtil);
		else
			fGpuLoadBar->SetValue(0);

		if (fValidMask & NVIDIA_STATS_VALID_MEM_UTIL)
			fMemLoadBar->SetValue(fStats->memUtil);
		else
			fMemLoadBar->SetValue(0);
	} else {
		fValidMask = 0;
		fGpuLoadBar->SetValue(0);
		fMemLoadBar->SetValue(0);
	}

	// Request redraw for text labels
	Invalidate();
}


void
NvidiaGPUView::Pulse()
{
	if (!IsHidden()) {
		Update();
	}
}


void
NvidiaGPUView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();

	// Draw semi-transparent background
	SetDrawingMode(B_OP_ALPHA);
	SetHighColor(40, 40, 40, 180);  // Dark gray with ~70% opacity
	FillRect(bounds);
	SetDrawingMode(B_OP_COPY);

	// Draw 1px gray border frame
	SetHighColor(100, 100, 100);
	StrokeRect(bounds);

	// Set font
	BFont font(be_plain_font);
	font.SetSize(11);
	SetFont(&font);
	SetHighColor(220, 220, 220);  // Light gray text

	char buf[64];
	float w;
	float labelX = 10;
	float topMargin = 12;  // Match Init()
	float clockX = LABEL_WIDTH + PROGRESS_BAR_WIDTH + 15;  // Right of progress bar
	float barY = topMargin + PROGRESS_BAR_HEIGHT - 3;

	// Row 1: GPU Load label + bar + GPU Clock
	DrawString("GPU:", BPoint(labelX, barY));
	if (fValidMask & NVIDIA_STATS_VALID_GPU_CLOCK) {
		snprintf(buf, sizeof(buf), "%u MHz", fGpuClockMHz);
	} else {
		snprintf(buf, sizeof(buf), "-- MHz");
	}
	DrawString(buf, BPoint(clockX, barY));

	// Row 2: Mem Load label + bar + Mem Clock
	barY += PROGRESS_BAR_HEIGHT + 8;
	DrawString("VRAM:", BPoint(labelX, barY));
	if (fValidMask & NVIDIA_STATS_VALID_MEM_CLOCK) {
		snprintf(buf, sizeof(buf), "%u MHz", fMemClockMHz);
	} else {
		snprintf(buf, sizeof(buf), "-- MHz");
	}
	DrawString(buf, BPoint(clockX, barY));

	// Row 3: Temperature and Power centered below
	float centerX = bounds.Width() / 2;
	float statsY = barY + PROGRESS_BAR_HEIGHT + 14;

	// Temperature icon (thermometer) - aligned with progress bar
	float tempIconX = LABEL_WIDTH + 10;  // Align with progress bar left edge
	float tempIconY = statsY - 14;

	// Draw thermometer icon - classic style with bulb and tube
	SetPenSize(1);
	// Outer tube (white outline)
	SetHighColor(220, 220, 220);
	FillRoundRect(BRect(tempIconX + 2, tempIconY, tempIconX + 8, tempIconY + 10), 2, 2);
	// Bulb at bottom (larger circle)
	FillEllipse(BPoint(tempIconX + 5, tempIconY + 13), 5, 5);

	// Inner dark background
	SetHighColor(60, 60, 60);
	FillRoundRect(BRect(tempIconX + 3, tempIconY + 1, tempIconX + 7, tempIconY + 9), 1, 1);
	FillEllipse(BPoint(tempIconX + 5, tempIconY + 13), 3.5, 3.5);

	// Mercury (red) - fills based on temp level
	SetHighColor(255, 60, 60);
	FillEllipse(BPoint(tempIconX + 5, tempIconY + 13), 3, 3);
	FillRect(BRect(tempIconX + 4, tempIconY + 4, tempIconX + 6, tempIconY + 13));

	// Tick marks on the side
	SetHighColor(220, 220, 220);
	StrokeLine(BPoint(tempIconX + 8, tempIconY + 2), BPoint(tempIconX + 10, tempIconY + 2));
	StrokeLine(BPoint(tempIconX + 8, tempIconY + 5), BPoint(tempIconX + 10, tempIconY + 5));
	StrokeLine(BPoint(tempIconX + 8, tempIconY + 8), BPoint(tempIconX + 10, tempIconY + 8));

	// Temperature text
	if (fValidMask & NVIDIA_STATS_VALID_TEMP) {
		int tempC = fGpuTemp >> 8;
		snprintf(buf, sizeof(buf), "%d\xC2\xB0" "C", tempC);  // degree symbol
		if (tempC >= TEMP_CRIT_THRESHOLD)
			SetHighColor(255, 50, 50);
		else if (tempC >= TEMP_WARN_THRESHOLD)
			SetHighColor(255, 180, 0);
		else
			SetHighColor(220, 220, 220);
	} else {
		snprintf(buf, sizeof(buf), "--\xC2\xB0" "C");
		SetHighColor(220, 220, 220);
	}
	DrawString(buf, BPoint(tempIconX + 18, statsY));
	SetHighColor(220, 220, 220);

	// Power icon (lightning bolt) - aligned to right edge of progress bar
	float progressBarRight = LABEL_WIDTH + 10 + PROGRESS_BAR_WIDTH;

	// Calculate power text first to right-align
	if (fValidMask & NVIDIA_STATS_VALID_POWER) {
		snprintf(buf, sizeof(buf), "%.1f W", fPowerMw / 1000.0f);
	} else {
		snprintf(buf, sizeof(buf), "-- W");
	}
	float powerTextWidth = StringWidth(buf);
	float iconWidth = 12;
	float powerIconX = progressBarRight - powerTextWidth - iconWidth - 5;
	float powerIconY = statsY - 14;

	// Draw lightning bolt icon (18px tall to match thermometer)
	SetHighColor(255, 200, 0);  // Golden yellow
	BPoint bolt[7];
	bolt[0] = BPoint(powerIconX + 9, powerIconY);
	bolt[1] = BPoint(powerIconX + 2, powerIconY + 9);
	bolt[2] = BPoint(powerIconX + 7, powerIconY + 9);
	bolt[3] = BPoint(powerIconX, powerIconY + 18);
	bolt[4] = BPoint(powerIconX + 8, powerIconY + 10);
	bolt[5] = BPoint(powerIconX + 4, powerIconY + 10);
	bolt[6] = BPoint(powerIconX + 9, powerIconY);
	FillPolygon(bolt, 7);

	// Add outline for definition
	SetHighColor(200, 150, 0);
	StrokePolygon(bolt, 7);

	// Power text - right aligned to progress bar
	SetHighColor(220, 220, 220);
	DrawString(buf, BPoint(progressBarRight - powerTextWidth, statsY));

	// Connection status if not connected
	if (fStats == NULL) {
		SetHighColor(180, 0, 0);
		font.SetSize(10);
		SetFont(&font);
		const char* msg = "(Driver not running)";
		w = StringWidth(msg);
		DrawString(msg, BPoint(centerX - w / 2, bounds.bottom - 8));
	}
}


void
NvidiaGPUView::MouseDown(BPoint where)
{
	BPoint cursor;
	uint32 buttons;
	MakeFocus(true);
	GetMouse(&cursor, &buttons, true);

	if (buttons & B_SECONDARY_MOUSE_BUTTON) {
		ConvertToScreen(&where);
		fPopupMenu->Go(where, true, false, true);
	}
}


void
NvidiaGPUView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_REPLICANT_PULSE:
			Update();
			break;

		case MSG_ABOUT:
		{
			BAlert* alert = new BAlert("About NvidiaGPU",
			                           "NvidiaGPU\n\n"
			                           "GPU monitoring tool for NVIDIA GPUs on Haiku.\n\n"
			                           "Shows GPU/Memory load, temperature, clocks, and power.\n\n"
			                           "Drag the handle to the Desktop to create a replicant.\n\n"
			                           "Part of the nvidia_gsp driver project.",
			                           "OK");
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go(NULL);
			break;
		}

		case MSG_QUIT:
			if (fIsReplicant) {
				// Remove replicant from shelf (Desktop)
				BDragger* dragger = dynamic_cast<BDragger*>(FindView("_dragger_"));
				if (dragger != NULL) {
					BMessage msg(B_TRASH_TARGET);
					msg.AddInt32("be:shelf_action", 1);  // Remove
					dragger->MessageReceived(&msg);
				}
			} else {
				be_app->PostMessage(B_QUIT_REQUESTED);
			}
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


NvidiaGPUView*
NvidiaGPUView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "NvidiaGPUView"))
		return NULL;
	return new NvidiaGPUView(archive);
}


status_t
NvidiaGPUView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", APP_SIGNATURE);
	if (status == B_OK)
		status = archive->AddString("class", "NvidiaGPUView");
	return status;
}
