/*
 * NvidiaGPUApp.cpp - Main application class
 */

#include "NvidiaGPUApp.h"
#include "NvidiaGPUWindow.h"
#include "NvidiaGPUView.h"
#include "Common.h"

#include <Alert.h>
#include <TextView.h>


NvidiaGPUApp::NvidiaGPUApp()
	: BApplication(APP_SIGNATURE),
	  fWindow(NULL)
{
}


NvidiaGPUApp::~NvidiaGPUApp()
{
}


void
NvidiaGPUApp::ReadyToRun()
{
	BRect frame(100, 100, 100 + VIEW_WIDTH, 100 + VIEW_HEIGHT);
	fWindow = new NvidiaGPUWindow(frame);
	fWindow->CenterOnScreen();
	fWindow->Show();
}


void
NvidiaGPUApp::AboutRequested()
{
	BString text = "NvidiaGPU\n\n"
	               "GPU monitoring tool for NVIDIA GPUs on Haiku.\n\n"
	               "Shows GPU/Memory load, temperature, clocks, and power.\n\n"
	               "Part of the nvidia_gsp driver project.";

	BAlert* alert = new BAlert("About NvidiaGPU", text.String(), "OK");

	BTextView* view = alert->TextView();
	BFont font;

	view->SetStylable(true);
	view->GetFont(&font);
	font.SetSize(18);
	font.SetFace(B_BOLD_FACE);
	view->SetFontAndColor(0, 9, &font);  // "NvidiaGPU"

	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


void
NvidiaGPUApp::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_ABOUT:
			AboutRequested();
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


int main()
{
	NvidiaGPUApp app;
	app.Run();
	return 0;
}
