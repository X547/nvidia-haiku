/*
 * MonitorWindow.h - window presenting NV00DE_SHARED_DATA as a live name/value
 * list, refreshed on a timer.
 */

#pragma once

#include <Window.h>
#include <MessageRunner.h>

#include "RusdSource.h"

class BColumnListView;
class BRow;
class BString;
class BTabView;


// One stable row per scalar pulled out of NV00DE_SHARED_DATA. The order here is
// only used to size the row-pointer array; the visible order and grouping are
// defined by the section table in the .cpp.
enum RowId {
	kRowPma = 0,
	kRowBar1,

	kRowGpuUtil,
	kRowMemUtil,
	kRowNvenc,
	kRowNvdec,
	kRowNvjpg,
	kRowNvofa,

	kRowGpcClk,
	kRowMemClk,
	kRowVidClk,
	kRowSmClk,

	kRowPstate,
	kRowThrottle,

	kRowPwrInstGpu,
	kRowPwrInstModule,
	kRowPwrInstCpu,
	kRowPwrAvgGpu,
	kRowPwrAvgModule,
	kRowPwrAvgMem,
	kRowPwrLimitReq,
	kRowPwrLimitEnf,

	kRowTempGpu,
	kRowTempHbm,

	kRowEccCorrVol,
	kRowEccCorrAgg,
	kRowEccUncVol,
	kRowEccUncAgg,

	kRowRemapCorr,
	kRowRemapUncorr,
	kRowRemapPending,
	kRowRemapFailure,

	kRowPcieGen,
	kRowPcieLinecode,
	kRowPcieCrc,
	kRowPcieNaks,
	kRowPcieL0s,
	kRowPcieCorr,
	kRowPcieNonfatal,
	kRowPcieFatal,
	kRowPcieUnsupReq,

	kRowCtxsw,
	kRowShadowErr,

	kRowCount
};


class MonitorWindow : public BWindow {
public:
	MonitorWindow(BRect frame);

	virtual void MessageReceived(BMessage* msg);
	virtual bool QuitRequested();

private:
	void BuildStatsRows();
	void UpdateStats();
	void SetRow(int32 id, const BString& value);
	void SetRowText(int32 id, const char* value);

	void BuildClientsView();
	void UpdateClients();

	BTabView* fTabView;
	BColumnListView* fStatsView;
	BColumnListView* fClientsView;
	BRow* fRows[kRowCount];
	RusdSource fSource;
	BMessageRunner fRunner;
};
