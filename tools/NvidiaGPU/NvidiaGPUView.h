/*
 * NvidiaGPUView.h - Main view displaying GPU information
 */

#ifndef NVIDIA_GPU_VIEW_H
#define NVIDIA_GPU_VIEW_H

#include <View.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <MenuItem.h>

#include "nvidia_gpu_stats.h"

class GPUProgressBar;

class NvidiaGPUView : public BView {
public:
	NvidiaGPUView(BRect frame);
	NvidiaGPUView(BMessage* archive);
	virtual ~NvidiaGPUView();

	virtual void AttachedToWindow();
	virtual void DetachedFromWindow();
	virtual void Draw(BRect updateRect);
	virtual void MouseDown(BPoint where);
	virtual void MessageReceived(BMessage* message);
	virtual void Pulse();

	static NvidiaGPUView* Instantiate(BMessage* archive);
	virtual status_t Archive(BMessage* archive, bool deep = true) const;

	void Update();

protected:
	void Init();
	void InitMenu();
	bool ConnectToStats();
	void DisconnectFromStats();

	GPUProgressBar* fGpuLoadBar;
	GPUProgressBar* fMemLoadBar;

	BPopUpMenu* fPopupMenu;
	BMenuItem* fAboutItem;
	BMenuItem* fQuitItem;

	// Shared area connection
	area_id fClonedArea;
	nvidia_gpu_stats_t* fStats;

	// Cached values for drawing
	int32 fGpuTemp;
	uint32 fGpuClockMHz;
	uint32 fMemClockMHz;
	uint32 fPowerMw;
	uint32 fValidMask;

	// For replicant
	BMessageRunner* fMessageRunner;
	bool fIsReplicant;
};

#endif // NVIDIA_GPU_VIEW_H
