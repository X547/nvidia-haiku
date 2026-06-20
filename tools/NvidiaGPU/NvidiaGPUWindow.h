/*
 * NvidiaGPUWindow.h - Main window for NvidiaGPU app
 */

#ifndef NVIDIA_GPU_WINDOW_H
#define NVIDIA_GPU_WINDOW_H

#include <Window.h>

class NvidiaGPUView;

class NvidiaGPUWindow : public BWindow {
public:
	NvidiaGPUWindow(BRect frame);
	virtual ~NvidiaGPUWindow();

	virtual void MessageReceived(BMessage* message);
	virtual bool QuitRequested();

private:
	NvidiaGPUView* fMainView;
};

#endif // NVIDIA_GPU_WINDOW_H
