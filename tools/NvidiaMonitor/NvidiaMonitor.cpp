/*
 * NvidiaMonitor.cpp - application entry point
 *
 * A small Haiku GUI tool that presents the GPU's NV00DE_SHARED_DATA (RUSD) as a
 * live, periodically refreshed name/value list. Talks to the NVIDIA RM directly
 * through nvrm_cpp_sdk, the same way the NvRmDemos do.
 */

#include <Application.h>

#include "MonitorWindow.h"


static const char* kSignature = "application/x-vnd.NvidiaMonitor";


class NvidiaMonitorApp : public BApplication {
public:
	NvidiaMonitorApp()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun()
	{
		MonitorWindow* window = new MonitorWindow(BRect(0, 0, 720, 560));
		window->CenterOnScreen();
		window->Show();
	}
};


int
main()
{
	NvidiaMonitorApp app;
	app.Run();
	return 0;
}
