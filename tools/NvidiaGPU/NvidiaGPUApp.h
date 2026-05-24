/*
 * NvidiaGPUApp.h - Main application class
 */

#ifndef NVIDIA_GPU_APP_H
#define NVIDIA_GPU_APP_H

#include <Application.h>

class NvidiaGPUWindow;

class NvidiaGPUApp : public BApplication {
public:
	NvidiaGPUApp();
	virtual ~NvidiaGPUApp();

	virtual void ReadyToRun();
	virtual void AboutRequested();
	virtual void MessageReceived(BMessage* message);

private:
	NvidiaGPUWindow* fWindow;
};

#endif // NVIDIA_GPU_APP_H
