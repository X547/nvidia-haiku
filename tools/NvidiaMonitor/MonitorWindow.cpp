/*
 * MonitorWindow.cpp - see MonitorWindow.h
 */

#include "MonitorWindow.h"
#include "Format.h"

#include <vector>

#include <image.h>
#include <OS.h>
#include <Alert.h>
#include <Application.h>
#include <LayoutBuilder.h>
#include <List.h>
#include <Path.h>
#include <String.h>
#include <TabView.h>
#include <private/interface/ColumnListView.h>
#include <private/interface/ColumnTypes.h>


enum {
	kNameCol = 0,
	kValueCol,
};

// Columns of the "Clients" (GPU processes) tab.
enum {
	clientNameCol = 0,
	clientPidCol,
	clientVramCol,
	clientVramPrivCol,
	clientVramSharedCol,
	clientPathCol,
};

enum {
	updateMsg = 'updt',
};

// 1 Hz refresh -- RUSD polled sections update at roughly this cadence.
static const bigtime_t kUpdateInterval = 1000000;


// Table describing the static layout: each section is a parent row with a set
// of value rows underneath. The RowId ties a value row to the field read in
// UpdateStats().
struct RowDesc {
	int32 id;
	const char* label;
};

struct Section {
	const char* name;
	const RowDesc* rows;
	size_t count;
};

static const RowDesc kMemoryRows[] = {
	{kRowPma,  "Video memory (PMA)"},
	{kRowBar1, "BAR1 aperture"},
};
static const RowDesc kUtilRows[] = {
	{kRowGpuUtil, "GPU"},
	{kRowMemUtil, "Memory"},
	{kRowNvenc,   "Video encode (NVENC)"},
	{kRowNvdec,   "Video decode (NVDEC)"},
	{kRowNvjpg,   "JPEG (NVJPG)"},
	{kRowNvofa,   "Optical flow (NVOFA)"},
};
static const RowDesc kClockRows[] = {
	{kRowGpcClk, "Graphics"},
	{kRowMemClk, "Memory"},
	{kRowVidClk, "Video"},
	{kRowSmClk,  "SM"},
};
static const RowDesc kPerfRows[] = {
	{kRowPstate,   "Current P-state"},
	{kRowThrottle, "Throttle reasons"},
};
static const RowDesc kPowerRows[] = {
	{kRowPwrInstGpu,    "GPU (instant)"},
	{kRowPwrInstModule, "Module (instant)"},
	{kRowPwrInstCpu,    "CPU (instant)"},
	{kRowPwrAvgGpu,     "GPU (average)"},
	{kRowPwrAvgModule,  "Module (average)"},
	{kRowPwrAvgMem,     "Memory (average)"},
	{kRowPwrLimitReq,   "Limit requested"},
	{kRowPwrLimitEnf,   "Limit enforced"},
};
static const RowDesc kTempRows[] = {
	{kRowTempGpu, "GPU"},
	{kRowTempHbm, "Memory (HBM)"},
};
static const RowDesc kEccRows[] = {
	{kRowEccCorrVol, "Corrected (volatile)"},
	{kRowEccCorrAgg, "Corrected (aggregate)"},
	{kRowEccUncVol,  "Uncorrected (volatile)"},
	{kRowEccUncAgg,  "Uncorrected (aggregate)"},
};
static const RowDesc kRemapRows[] = {
	{kRowRemapCorr,    "Correctable rows"},
	{kRowRemapUncorr,  "Uncorrectable rows"},
	{kRowRemapPending, "Remap pending"},
	{kRowRemapFailure, "Failure occurred"},
};
static const RowDesc kPcieRows[] = {
	{kRowPcieGen,      "Gen/link info"},
	{kRowPcieLinecode, "Linecode errors"},
	{kRowPcieCrc,      "CRC errors"},
	{kRowPcieNaks,     "NAKs received"},
	{kRowPcieL0s,      "Failed L0s exits"},
	{kRowPcieCorr,     "Correctable errors"},
	{kRowPcieNonfatal, "Non-fatal errors"},
	{kRowPcieFatal,    "Fatal errors"},
	{kRowPcieUnsupReq, "Unsupported requests"},
};
static const RowDesc kMiscRows[] = {
	{kRowCtxsw,     "Ctxsw logging"},
	{kRowShadowErr, "Shadow ERR_CONT"},
};

#define SECTION(name, arr) { name, arr, sizeof(arr) / sizeof(arr[0]) }
static const Section kSections[] = {
	SECTION("Memory", kMemoryRows),
	SECTION("Utilization", kUtilRows),
	SECTION("Clocks", kClockRows),
	SECTION("Performance", kPerfRows),
	SECTION("Power", kPowerRows),
	SECTION("Temperature", kTempRows),
	SECTION("ECC errors (total)", kEccRows),
	SECTION("Row remapping", kRemapRows),
	SECTION("PCIe", kPcieRows),
	SECTION("Misc", kMiscRows),
};
#undef SECTION


static void FormatPower(BString& out, uint32 milliwatts)
{
	out.SetToFormat("%.2f W", double(milliwatts) / 1000.0);
}


static void FormatTemp(BString& out, NvTemp temp)
{
	// NvTemp is signed 24.8 fixed point in degrees Celsius.
	out.SetToFormat("%.1f \xC2\xB0""C", double(temp) / 256.0);
}


static void ThrottleString(NvU32 mask, BString& out)
{
	static const struct {
		NvU32 bit;
		const char* name;
	} kReasons[] = {
		{RUSD_CLK_THROTTLE_REASON_GPU_IDLE,                  "Idle"},
		{RUSD_CLK_THROTTLE_REASON_APPLICATION_CLOCK_SETTING, "App clock"},
		{RUSD_CLK_THROTTLE_REASON_SW_POWER_CAP,              "SW power cap"},
		{RUSD_CLK_THROTTLE_REASON_HW_SLOWDOWN,               "HW slowdown"},
		{RUSD_CLK_THROTTLE_REASON_SYNC_BOOST,                "Sync boost"},
		{RUSD_CLK_THROTTLE_REASON_SW_THERMAL_SLOWDOWN,       "SW thermal"},
		{RUSD_CLK_THROTTLE_REASON_HW_THERMAL_SLOWDOWN,       "HW thermal"},
		{RUSD_CLK_THROTTLE_REASON_HW_POWER_BRAKES_SLOWDOWN,  "Power brake"},
		{RUSD_CLK_THROTTLE_REASON_DISPLAY_CLOCK_SETTING,     "Display clock"},
	};

	out.SetTo("");
	for (size_t i = 0; i < sizeof(kReasons) / sizeof(kReasons[0]); i++) {
		if ((mask & kReasons[i].bit) != 0) {
			if (out.Length() > 0)
				out += ", ";
			out += kReasons[i].name;
		}
	}
	if (out.Length() == 0)
		out.SetTo("None");
}


MonitorWindow::MonitorWindow(BRect frame)
	:
	BWindow(frame, "NVIDIA Monitor", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE),
	fTabView(NULL),
	fStatsView(NULL),
	fClientsView(NULL),
	fRunner(BMessenger(this), BMessage(updateMsg), kUpdateInterval)
{
	for (int32 i = 0; i < kRowCount; i++)
		fRows[i] = NULL;

	fStatsView = new BColumnListView("Stats", B_NAVIGABLE);
	fStatsView->AddColumn(new BStringColumn("Name", 200, 50, 600, B_TRUNCATE_END), kNameCol);
	fStatsView->AddColumn(new BStringColumn("Value", 200, 50, 1024, B_TRUNCATE_END,
		B_ALIGN_RIGHT), kValueCol);
	BuildStatsRows();

	BuildClientsView();

	fTabView = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	fTabView->SetBorder(B_NO_BORDER);
	fTabView->AddTab(fStatsView);
	fTabView->AddTab(fClientsView);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fTabView)
	.End();

	UpdateStats();
	UpdateClients();

	if (!fSource.IsAvailable()) {
		BString text("Could not read GPU shared data (NV00DE_SHARED_DATA):\n");
		text += fSource.Error();
		BAlert* alert = new BAlert("NVIDIA Monitor", text.String(), "OK");
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
		alert->Go(NULL);
	}
}


void
MonitorWindow::BuildStatsRows()
{
	for (size_t s = 0; s < sizeof(kSections) / sizeof(kSections[0]); s++) {
		const Section& section = kSections[s];

		BRow* parent = new BRow();
		parent->SetField(new BStringField(section.name), kNameCol);
		fStatsView->AddRow(parent);

		for (size_t r = 0; r < section.count; r++) {
			const RowDesc& desc = section.rows[r];
			BRow* row = new BRow();
			row->SetField(new BStringField(desc.label), kNameCol);
			row->SetField(new BStringField(""), kValueCol);
			fStatsView->AddRow(row, parent);
			fRows[desc.id] = row;
		}

		fStatsView->ExpandOrCollapse(parent, true);
	}
}


void
MonitorWindow::SetRow(int32 id, const BString& value)
{
	SetRowText(id, value.String());
}


void
MonitorWindow::SetRowText(int32 id, const char* value)
{
	if (id < 0 || id >= kRowCount || fRows[id] == NULL)
		return;
	fRows[id]->SetField(new BStringField(value), kValueCol);
	fStatsView->UpdateRow(fRows[id]);
}


void
MonitorWindow::UpdateStats()
{
	// Default everything to "not available"; successful reads overwrite below.
	for (int32 i = 0; i < kRowCount; i++)
		SetRowText(i, "\xE2\x80\x94");	// em dash

	if (!fSource.IsAvailable())
		return;

	const NV00DE_SHARED_DATA* d = fSource.Data();
	BString s;

	// --- Memory: finite resources, formatted used/total like the rest of the
	// Haiku system tools. PMA is in bytes; BAR1 in KB. ---
	RUSD_PMA_MEMORY_INFO pma;
	if (fSource.Read(d->pmaMemoryInfo, pma) && pma.totalPmaMemory > 0) {
		GetUsedMaxSize(s, pma.totalPmaMemory - pma.freePmaMemory, pma.totalPmaMemory);
		SetRow(kRowPma, s);
	}

	RUSD_BAR1_MEMORY_INFO bar1;
	if (fSource.Read(d->bar1MemoryInfo, bar1) && bar1.bar1Size > 0) {
		uint64 total = (uint64)bar1.bar1Size * 1024;
		uint64 avail = (uint64)bar1.bar1AvailSize * 1024;
		GetUsedMaxSize(s, total - avail, total);
		SetRow(kRowBar1, s);
	}

	// --- Utilization ---
	RUSD_PERF_DEVICE_UTILIZATION util;
	if (fSource.Read(d->perfDevUtil, util)) {
		s.SetToFormat("%" B_PRIu32 " %%", util.info.gpuPercentBusy); SetRow(kRowGpuUtil, s);
		s.SetToFormat("%" B_PRIu32 " %%", util.info.memoryPercentBusy); SetRow(kRowMemUtil, s);
		s.SetToFormat("%" B_PRIu32 " %%",
			util.info.engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVENC].clkPercentBusy);
		SetRow(kRowNvenc, s);
		s.SetToFormat("%" B_PRIu32 " %%",
			util.info.engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVDEC].clkPercentBusy);
		SetRow(kRowNvdec, s);
		s.SetToFormat("%" B_PRIu32 " %%",
			util.info.engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVJPG].clkPercentBusy);
		SetRow(kRowNvjpg, s);
		s.SetToFormat("%" B_PRIu32 " %%",
			util.info.engUtil[RUSD_ENG_UTILIZATION_VID_ENG_NVOFA].clkPercentBusy);
		SetRow(kRowNvofa, s);
	}

	// --- Clocks ---
	RUSD_CLK_PUBLIC_DOMAIN_INFOS clk;
	if (fSource.Read(d->clkPublicDomainInfos, clk)) {
		s.SetToFormat("%" B_PRIu32 " MHz", clk.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].targetClkMHz);
		SetRow(kRowGpcClk, s);
		s.SetToFormat("%" B_PRIu32 " MHz", clk.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].targetClkMHz);
		SetRow(kRowMemClk, s);
		s.SetToFormat("%" B_PRIu32 " MHz", clk.info[RUSD_CLK_PUBLIC_DOMAIN_VIDEO].targetClkMHz);
		SetRow(kRowVidClk, s);
		s.SetToFormat("%" B_PRIu32 " MHz", clk.info[RUSD_CLK_PUBLIC_DOMAIN_SM].targetClkMHz);
		SetRow(kRowSmClk, s);
	}

	// --- Performance ---
	RUSD_PERF_CURRENT_PSTATE pstate;
	if (fSource.Read(d->perfCurrentPstate, pstate)) {
		s.SetToFormat("P%" B_PRIu32, pstate.currentPstate);
		SetRow(kRowPstate, s);
	}

	RUSD_CLK_THROTTLE_REASON throttle;
	if (fSource.Read(d->clkThrottleReason, throttle)) {
		ThrottleString(throttle.reasonMask, s);
		SetRow(kRowThrottle, s);
	}

	// --- Power ---
	RUSD_INST_POWER_USAGE instPwr;
	if (fSource.Read(d->instPowerUsage, instPwr)) {
		FormatPower(s, instPwr.info.instGpuPower); SetRow(kRowPwrInstGpu, s);
		FormatPower(s, instPwr.info.instModulePower); SetRow(kRowPwrInstModule, s);
		FormatPower(s, instPwr.info.instCpuPower); SetRow(kRowPwrInstCpu, s);
	}

	RUSD_AVG_POWER_USAGE avgPwr;
	if (fSource.Read(d->avgPowerUsage, avgPwr)) {
		FormatPower(s, avgPwr.info.averageGpuPower); SetRow(kRowPwrAvgGpu, s);
		FormatPower(s, avgPwr.info.averageModulePower); SetRow(kRowPwrAvgModule, s);
		FormatPower(s, avgPwr.info.averageMemoryPower); SetRow(kRowPwrAvgMem, s);
	}

	RUSD_POWER_LIMITS pwrLimit;
	if (fSource.Read(d->powerLimitGpu, pwrLimit)) {
		FormatPower(s, pwrLimit.info.requestedmW); SetRow(kRowPwrLimitReq, s);
		FormatPower(s, pwrLimit.info.enforcedmW); SetRow(kRowPwrLimitEnf, s);
	}

	// --- Temperature ---
	RUSD_TEMPERATURE temp;
	if (fSource.Read(d->temperatures[RUSD_TEMPERATURE_TYPE_GPU], temp)) {
		FormatTemp(s, temp.temperature);
		SetRow(kRowTempGpu, s);
	}
	if (fSource.Read(d->temperatures[RUSD_TEMPERATURE_TYPE_HBM], temp)) {
		FormatTemp(s, temp.temperature);
		SetRow(kRowTempHbm, s);
	}

	// --- ECC error counts (TOTAL bucket) ---
	RUSD_MEM_ECC ecc;
	if (fSource.Read(d->memEcc, ecc)) {
		const RUSD_MEM_ERROR_COUNTS& c = ecc.count[RUSD_MEMORY_ERROR_TYPE_TOTAL];
		s.SetToFormat("%" B_PRIu64, c.correctedVolatile); SetRow(kRowEccCorrVol, s);
		s.SetToFormat("%" B_PRIu64, c.correctedAggregate); SetRow(kRowEccCorrAgg, s);
		s.SetToFormat("%" B_PRIu64, c.uncorrectedVolatile); SetRow(kRowEccUncVol, s);
		s.SetToFormat("%" B_PRIu64, c.uncorrectedAggregate); SetRow(kRowEccUncAgg, s);
	}

	// --- Row remapping ---
	RUSD_MEM_ROW_REMAP remap;
	if (fSource.Read(d->memRowRemap, remap)) {
		s.SetToFormat("%" B_PRIu32, remap.info.correctableRows); SetRow(kRowRemapCorr, s);
		s.SetToFormat("%" B_PRIu32, remap.info.uncorrectableRows); SetRow(kRowRemapUncorr, s);
		SetRowText(kRowRemapPending, remap.info.isPending ? "Yes" : "No");
		SetRowText(kRowRemapFailure, remap.info.hasFailureOccurred ? "Yes" : "No");
	}

	// --- PCIe counters ---
	RUSD_PCIE_DATA pcie;
	if (fSource.Read(d->pciBusData, pcie)) {
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GEN_INFO]);
		SetRow(kRowPcieGen, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_LINECODE_ERRORS]);
		SetRow(kRowPcieLinecode, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_CRC_ERRORS]);
		SetRow(kRowPcieCrc, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_NAKS_RECEIVED]);
		SetRow(kRowPcieNaks, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_FAILED_L0S_EXITS]);
		SetRow(kRowPcieL0s, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_CORRECTABLE_ERRORS]);
		SetRow(kRowPcieCorr, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_NONFATAL_ERRORS]);
		SetRow(kRowPcieNonfatal, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_FATAL_ERRORS]);
		SetRow(kRowPcieFatal, s);
		s.SetToFormat("%" B_PRIu32, pcie.info.data[RUSD_BUS_DATA_PCIE_GPU_LINK_UNSUPPORTED_REQUESTS]);
		SetRow(kRowPcieUnsupReq, s);
	}

	// --- Misc ---
	RUSD_GR_INFO gr;
	if (fSource.Read(d->grInfo, gr))
		SetRowText(kRowCtxsw, gr.bCtxswLoggingEnabled ? "Enabled" : "Disabled");

	RUSD_SHADOW_ERR_CONT shadow;
	if (fSource.Read(d->shadowErrCont, shadow)) {
		s.SetToFormat("0x%08" B_PRIx32, shadow.shadowErrContVal);
		SetRow(kRowShadowErr, s);
	}
}


// Resolve a process id (RM client PID == Haiku team id) to a display name and
// executable path using its main image, like SystemManager's Teams tab.
static void
ResolveTeam(uint32 team, BString& name, BString& path)
{
	path.SetTo("");

	// team id 0 is B_CURRENT_TEAM, not a real process; such RM clients have no
	// userland team. Don't resolve them against the calling process.
	if (team == 0) {
		name.SetTo("(kernel)");
		return;
	}

	image_info imageInfo;
	int32 cookie = 0;
	if (get_next_image_info((team_id)team, &cookie, &imageInfo) == B_OK) {
		path.SetTo(imageInfo.name);
		BPath p(imageInfo.name);
		name.SetTo(p.Leaf());
	} else {
		name.SetToFormat("team %" B_PRIu32, team);
	}
}


void
MonitorWindow::BuildClientsView()
{
	fClientsView = new BColumnListView("Clients", B_NAVIGABLE);
	fClientsView->AddColumn(new BStringColumn("Name", 160, 50, 400, B_TRUNCATE_END),
		clientNameCol);
	fClientsView->AddColumn(new BIntegerColumn("PID", 60, 40, 120, B_ALIGN_RIGHT),
		clientPidCol);
	fClientsView->AddColumn(new BStringColumn("VRAM", 90, 50, 200, B_TRUNCATE_END,
		B_ALIGN_RIGHT), clientVramCol);
	fClientsView->AddColumn(new BStringColumn("Private", 90, 50, 200, B_TRUNCATE_END,
		B_ALIGN_RIGHT), clientVramPrivCol);
	fClientsView->AddColumn(new BStringColumn("Shared", 90, 50, 200, B_TRUNCATE_END,
		B_ALIGN_RIGHT), clientVramSharedCol);
	fClientsView->AddColumn(new BStringColumn("Path", 300, 50, 1024, B_TRUNCATE_MIDDLE),
		clientPathCol);
}


void
MonitorWindow::UpdateClients()
{
	std::vector<GpuClientInfo> clients;
	if (!fSource.QueryClients(clients)) {
		fClientsView->Clear();
		return;
	}

	// Match existing rows by PID so selection/scroll survive refreshes; add new
	// processes and drop ones that have exited.
	BList prevRows;
	for (int32 i = 0; i < fClientsView->CountRows(); i++)
		prevRows.AddItem(fClientsView->RowAt(i));

	BString str, name, path;
	for (size_t c = 0; c < clients.size(); c++) {
		const GpuClientInfo& client = clients[c];

		BRow* row = NULL;
		for (int32 i = 0; i < prevRows.CountItems(); i++) {
			BRow* candidate = (BRow*)prevRows.ItemAt(i);
			if ((uint32)((BIntegerField*)candidate->GetField(clientPidCol))->Value()
					== client.pid) {
				row = candidate;
				prevRows.RemoveItem(i);
				break;
			}
		}

		if (row == NULL) {
			row = new BRow();
			row->SetField(new BStringField(""), clientNameCol);
			row->SetField(new BIntegerField((int32)client.pid), clientPidCol);
			row->SetField(new BStringField(""), clientVramCol);
			row->SetField(new BStringField(""), clientVramPrivCol);
			row->SetField(new BStringField(""), clientVramSharedCol);
			row->SetField(new BStringField(""), clientPathCol);
			fClientsView->AddRow(row);
		}

		ResolveTeam(client.pid, name, path);
		((BStringField*)row->GetField(clientNameCol))->SetString(name.String());
		((BStringField*)row->GetField(clientPathCol))->SetString(path.String());
		GetSizeString(str, client.vramTotal);
		((BStringField*)row->GetField(clientVramCol))->SetString(str.String());
		GetSizeString(str, client.vramPrivate);
		((BStringField*)row->GetField(clientVramPrivCol))->SetString(str.String());
		GetSizeString(str, client.vramShared);
		((BStringField*)row->GetField(clientVramSharedCol))->SetString(str.String());
		fClientsView->UpdateRow(row);
	}

	for (int32 i = 0; i < prevRows.CountItems(); i++) {
		BRow* row = (BRow*)prevRows.ItemAt(i);
		fClientsView->RemoveRow(row);
		delete row;
	}
}


void
MonitorWindow::MessageReceived(BMessage* msg)
{
	switch (msg->what) {
		case updateMsg:
		{
			// Refresh only the visible tab to avoid needless RM control calls.
			BTab* tab = fTabView->TabAt(fTabView->Selection());
			BView* view = (tab != NULL) ? tab->View() : NULL;
			if (view == fClientsView)
				UpdateClients();
			else
				UpdateStats();
			return;
		}
		default:
			BWindow::MessageReceived(msg);
	}
}


bool
MonitorWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}
