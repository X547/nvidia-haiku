/*
 * NvidiaGPUWindow.cpp - Main window for NvidiaGPU app
 */

#include "NvidiaGPUWindow.h"
#include "NvidiaGPUView.h"
#include "Common.h"

#include <Application.h>


NvidiaGPUWindow::NvidiaGPUWindow(BRect frame)
	: BWindow(frame, "NvidiaGPU", B_TITLED_WINDOW,
	          B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_QUIT_ON_WINDOW_CLOSE)
{
	SetPulseRate(500000);  // 500ms update rate

	fMainView = new NvidiaGPUView(Bounds());
	AddChild(fMainView);
}


NvidiaGPUWindow::~NvidiaGPUWindow()
{
}


void
NvidiaGPUWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_QUIT:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
NvidiaGPUWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
