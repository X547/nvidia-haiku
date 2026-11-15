/* See hevc_parse.h. Section numbers are from ITU-T H.265 (09/2023). */

#include "hevc_parse.h"

#include <limits.h>
#include <string.h>

/* Check unsigned syntax before signed conversion and arithmetic. Failure is
 * sticky so callers can reject a whole parameter set without partial use. */
static int
readUnsigned(H264Bits *br, uint32_t maximum)
{
	uint32_t value = h264UE(br);
	if (value > maximum) {
		br->failed = true;
		return 0;
	}
	return (int)value;
}

static bool
validHeader(const uint8_t *rbsp, size_t size, int type)
{
	return rbsp != NULL && size >= 3 && !(rbsp[0] & 0x81)
		&& !(rbsp[1] & 0xf8) && (rbsp[1] & 7) != 0
		&& ((rbsp[0] >> 1) & 63) == type;
}

static int
ceilLog2(int value)
{
	int bits = 0;
	while ((1 << bits) < value)
		bits++;
	return bits;
}

static size_t
bitPosition(const H264Bits *br)
{
	return br->bytePos * 8 + (size_t)br->bitPos;
}

/* Whole parameter sets retain their stop bit; slice headers retain data.
 * Also propagate the shared reader's sticky syntax/truncation failure. */
static bool
overran(const H264Bits *br)
{
	return br->failed || br->bytePos >= br->size;
}

/* ------------------------------------------------------- scaling lists */

/* 6.5.3: the up-right diagonal scan of a square block, as raster positions. */
static void
diagonalScan(int size, uint8_t *raster)
{
	int i = 0, x = 0, y = 0;
	while (i < size * size) {
		while (y >= 0) {
			if (x < size && y < size)
				raster[i++] = (uint8_t)(y * size + x);
			y--;
			x++;
		}
		y = x;
		x = 0;
	}
}

/* Table 7-6, in the order they are sent. */
static const uint8_t kDefaultIntra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};
static const uint8_t kDefaultInter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

static uint8_t *
scalingListFor(HevcScalingList *lists, int sizeId, int matrixId)
{
	switch (sizeId) {
	case 0:		return lists->list4x4[matrixId];
	case 1:		return lists->list8x8[matrixId];
	case 2:		return lists->list16x16[matrixId];
	default:	return lists->list32x32[matrixId];
	}
}

static void
setDefaultList(HevcScalingList *lists, int sizeId, int matrixId)
{
	uint8_t *list = scalingListFor(lists, sizeId, matrixId);
	if (sizeId == 0) {
		memset(list, 16, 16);
		return;
	}
	uint8_t raster[64];
	diagonalScan(8, raster);
	const uint8_t *source = matrixId < 3 ? kDefaultIntra : kDefaultInter;
	for (int i = 0; i < 64; i++)
		list[raster[i]] = source[i];
	if (sizeId == 2)
		lists->dc16x16[matrixId] = 16;
	else if (sizeId == 3)
		lists->dc32x32[matrixId] = 16;
}

static void
setDefaultScaling(HevcScalingList *lists)
{
	memset(lists, 0, sizeof(*lists));
	for (int sizeId = 0; sizeId < 4; sizeId++) {
		for (int matrixId = 0; matrixId < 6; matrixId++)
			setDefaultList(lists, sizeId, matrixId);
	}
}

/* 7.3.4 */
static bool
parseScalingListData(H264Bits *br, HevcScalingList *lists)
{
	uint8_t raster4[16], raster8[64];
	diagonalScan(4, raster4);
	diagonalScan(8, raster8);
	setDefaultScaling(lists);

	for (int sizeId = 0; sizeId < 4; sizeId++) {
		for (int matrixId = 0; matrixId < 6;
				matrixId += (sizeId == 3) ? 3 : 1) {
			uint8_t *list = scalingListFor(lists, sizeId, matrixId);
			int coefficients = sizeId == 0 ? 16 : 64;
			if (!h264Bit(br)) {
				/* Predicted: a copy of an earlier list, or the default. */
				uint32_t delta = h264UE(br);
				if (delta > (uint32_t)matrixId / (sizeId == 3 ? 3 : 1))
					return false;
				if (delta == 0) {
					setDefaultList(lists, sizeId, matrixId);
					continue;
				}
				int reference = matrixId - (int)delta * (sizeId == 3 ? 3 : 1);
				memcpy(list, scalingListFor(lists, sizeId, reference),
					coefficients);
				if (sizeId == 2)
					lists->dc16x16[matrixId] = lists->dc16x16[reference];
				else if (sizeId == 3)
					lists->dc32x32[matrixId] = lists->dc32x32[reference];
				continue;
			}
			int next = 8;
			if (sizeId > 1) {
				int dc = h264SE(br);
				if (dc < -7 || dc > 247)
					return false;
				dc += 8;
				next = dc;
				if (sizeId == 2)
					lists->dc16x16[matrixId] = (uint8_t)dc;
				else
					lists->dc32x32[matrixId] = (uint8_t)dc;
			}
			const uint8_t *raster = sizeId == 0 ? raster4 : raster8;
			for (int i = 0; i < coefficients; i++) {
				int delta = h264SE(br);
				if (delta < -128 || delta > 127)
					return false;
				next = (next + delta + 256) % 256;
				list[raster[i]] = (uint8_t)next;
			}
		}
	}
	return !overran(br);
}

/* ------------------------------------------------------- general parts */

/* 7.3.3 */
static void
parseProfileTierLevel(H264Bits *br, int maxSubLayersMinus1, HevcSps *sps)
{
	h264Bits(br, 2);			/* general_profile_space */
	sps->tierFlag = (int)h264Bit(br);
	sps->profileIdc = (int)h264Bits(br, 5);
	h264Bits(br, 32);			/* compatibility flags */
	h264Bits(br, 4);			/* progressive, interlaced,
						   non-packed, frame only */
	h264Bits(br, 32);			/* 43 bits of constraints */
	h264Bits(br, 11);
	h264Bit(br);				/* inbld / reserved */
	sps->levelIdc = (int)h264Bits(br, 8);

	bool profilePresent[8] = { false }, levelPresent[8] = { false };
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		profilePresent[i] = h264Bit(br);
		levelPresent[i] = h264Bit(br);
	}
	if (maxSubLayersMinus1 > 0) {
		for (int i = maxSubLayersMinus1; i < 8; i++)
			h264Bits(br, 2);
	}
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		if (profilePresent[i]) {
			h264Bits(br, 32);
			h264Bits(br, 32);
			h264Bits(br, 24);	/* 88 bits */
		}
		if (levelPresent[i])
			h264Bits(br, 8);
	}
}

/* 7.3.7, and the derivation in 7.4.8. `index` is the set being read; when
 * it equals the number of sets in the SPS, it is a slice's own. */
static bool
parseShortTermSet(H264Bits *br, const HevcSps *sps, int index,
	HevcShortTermSet *set)
{
	memset(set, 0, sizeof(*set));
	bool interPrediction = index != 0 && h264Bit(br);
	if (interPrediction) {
		int deltaIndex = 1;
		if (index == sps->numShortTermSets)
			deltaIndex = readUnsigned(br, index - 1) + 1;
		if (deltaIndex > index)
			return false;
		const HevcShortTermSet *reference = &sps->shortTerm[index - deltaIndex];
		int sign = (int)h264Bit(br);
		int absDelta = readUnsigned(br, 32767) + 1;
		if (absDelta > 32768)
			return false;
		int deltaRps = (1 - 2 * sign) * absDelta;
		int total = reference->numNegative + reference->numPositive;
		set->numDeltaPocsOfRefRpsIdx = total;

		bool used[2 * HEVC_MAX_DELTA_POCS + 1];
		bool useDelta[2 * HEVC_MAX_DELTA_POCS + 1];
		for (int j = 0; j <= total; j++) {
			used[j] = h264Bit(br);
			useDelta[j] = used[j] ? true : h264Bit(br);
		}

		/* 7-61 */
		int i = 0;
		for (int j = reference->numPositive - 1; j >= 0; j--) {
			int deltaPoc = reference->deltaPoc[1][j] + deltaRps;
			if (deltaPoc < 0 && useDelta[reference->numNegative + j]) {
				if (i >= HEVC_MAX_DELTA_POCS)
					return false;
				set->deltaPoc[0][i] = deltaPoc;
				set->used[0][i++] = used[reference->numNegative + j];
			}
		}
		if (deltaRps < 0 && useDelta[total]) {
			if (i >= HEVC_MAX_DELTA_POCS)
				return false;
			set->deltaPoc[0][i] = deltaRps;
			set->used[0][i++] = used[total];
		}
		for (int j = 0; j < reference->numNegative; j++) {
			int deltaPoc = reference->deltaPoc[0][j] + deltaRps;
			if (deltaPoc < 0 && useDelta[j]) {
				if (i >= HEVC_MAX_DELTA_POCS)
					return false;
				set->deltaPoc[0][i] = deltaPoc;
				set->used[0][i++] = used[j];
			}
		}
		set->numNegative = i;

		/* 7-62 */
		i = 0;
		for (int j = reference->numNegative - 1; j >= 0; j--) {
			int deltaPoc = reference->deltaPoc[0][j] + deltaRps;
			if (deltaPoc > 0 && useDelta[j]) {
				if (i >= HEVC_MAX_DELTA_POCS)
					return false;
				set->deltaPoc[1][i] = deltaPoc;
				set->used[1][i++] = used[j];
			}
		}
		if (deltaRps > 0 && useDelta[total]) {
			if (i >= HEVC_MAX_DELTA_POCS)
				return false;
			set->deltaPoc[1][i] = deltaRps;
			set->used[1][i++] = used[total];
		}
		for (int j = 0; j < reference->numPositive; j++) {
			int deltaPoc = reference->deltaPoc[1][j] + deltaRps;
			if (deltaPoc > 0 && useDelta[reference->numNegative + j]) {
				if (i >= HEVC_MAX_DELTA_POCS)
					return false;
				set->deltaPoc[1][i] = deltaPoc;
				set->used[1][i++] = used[reference->numNegative + j];
			}
		}
		set->numPositive = i;
		return !overran(br) && set->numNegative + set->numPositive <= HEVC_MAX_DELTA_POCS;
	}

	uint32_t negative = h264UE(br);
	uint32_t positive = h264UE(br);
	if (negative > HEVC_MAX_DELTA_POCS || positive > HEVC_MAX_DELTA_POCS
		|| negative + positive > HEVC_MAX_DELTA_POCS) {
		return false;
	}
	set->numNegative = (int)negative;
	set->numPositive = (int)positive;
	int poc = 0;
	for (uint32_t i = 0; i < negative; i++) {
		poc -= readUnsigned(br, 32767) + 1;
		set->deltaPoc[0][i] = poc;
		set->used[0][i] = h264Bit(br);
	}
	poc = 0;
	for (uint32_t i = 0; i < positive; i++) {
		poc += readUnsigned(br, 32767) + 1;
		set->deltaPoc[1][i] = poc;
		set->used[1][i] = h264Bit(br);
	}
	return !overran(br);
}

/* E.2.3 */
static void
skipSubLayerHrd(H264Bits *br, int cpbCount, bool subPicParams)
{
	for (int i = 0; i < cpbCount; i++) {
		h264UE(br);
		h264UE(br);
		if (subPicParams) {
			h264UE(br);
			h264UE(br);
		}
		h264Bit(br);
	}
}

/* E.2.2 */
static bool
skipHrd(H264Bits *br, bool commonInfoPresent, int maxSubLayersMinus1)
{
	bool nal = false, vcl = false, subPicParams = false;
	if (commonInfoPresent) {
		nal = h264Bit(br);
		vcl = h264Bit(br);
		if (nal || vcl) {
			subPicParams = h264Bit(br);
			if (subPicParams) {
				h264Bits(br, 8);
				h264Bits(br, 5);
				h264Bit(br);
				h264Bits(br, 5);
			}
			h264Bits(br, 4);
			h264Bits(br, 4);
			if (subPicParams)
				h264Bits(br, 4);
			h264Bits(br, 5);
			h264Bits(br, 5);
			h264Bits(br, 5);
		}
	}
	for (int i = 0; i <= maxSubLayersMinus1; i++) {
		bool fixedGeneral = h264Bit(br);
		bool fixedWithinCvs = true;
		bool lowDelay = false;
		if (!fixedGeneral)
			fixedWithinCvs = h264Bit(br);
		if (fixedWithinCvs)
			h264UE(br);
		else
			lowDelay = h264Bit(br);
		int cpbCount = 1;
		if (!lowDelay) {
			uint32_t count = h264UE(br);
			if (count > 31)
				return false;
			cpbCount = (int)count + 1;
		}
		if (nal)
			skipSubLayerHrd(br, cpbCount, subPicParams);
		if (vcl)
			skipSubLayerHrd(br, cpbCount, subPicParams);
	}
	return !overran(br);
}

/* E.2.1 */
static bool
parseVui(H264Bits *br, HevcSps *sps)
{
	if (h264Bit(br)) {			/* aspect_ratio_info_present_flag */
		if (h264Bits(br, 8) == 255) {
			h264Bits(br, 16);
			h264Bits(br, 16);
		}
	}
	if (h264Bit(br))			/* overscan_info_present_flag */
		h264Bit(br);
	if (h264Bit(br)) {			/* video_signal_type_present_flag */
		h264Bits(br, 3);
		sps->fullRange = (int)h264Bit(br);
		if (h264Bit(br)) {
			sps->colourPrimaries = (int)h264Bits(br, 8);
			sps->transferCharacteristics = (int)h264Bits(br, 8);
			sps->matrixCoefficients = (int)h264Bits(br, 8);
		}
	}
	if (h264Bit(br)) {			/* chroma_loc_info_present_flag */
		h264UE(br);
		h264UE(br);
	}
	h264Bit(br);				/* neutral_chroma_indication_flag */
	sps->fieldSeq = (int)h264Bit(br);
	h264Bit(br);				/* frame_field_info_present_flag */
	if (h264Bit(br)) {			/* default_display_window_flag */
		h264UE(br);
		h264UE(br);
		h264UE(br);
		h264UE(br);
	}
	if (h264Bit(br)) {			/* vui_timing_info_present_flag */
		h264Bits(br, 32);
		h264Bits(br, 32);
		if (h264Bit(br))
			h264UE(br);
		if (h264Bit(br)) {
			if (!skipHrd(br, true, sps->maxSubLayersMinus1))
				return false;
		}
	}
	if (h264Bit(br)) {			/* bitstream_restriction_flag */
		h264Bit(br);
		h264Bit(br);
		h264Bit(br);
		h264UE(br);
		h264UE(br);
		h264UE(br);
		h264UE(br);
		h264UE(br);
	}
	return !overran(br);
}

/* ----------------------------------------------------------------- SPS */

/* 7.3.2.2 */
bool
hevcParseSps(const uint8_t *rbsp, size_t size, HevcSps *sps)
{
	H264Bits br;
	if (!validHeader(rbsp, size, HEVC_NAL_SPS))
		return false;
	h264BitsInit(&br, rbsp + 2, size - 2);	/* past the NAL unit header */
	memset(sps, 0, sizeof(*sps));
	sps->colourPrimaries = 2;
	sps->transferCharacteristics = 2;
	sps->matrixCoefficients = 2;

	h264Bits(&br, 4);			/* sps_video_parameter_set_id */
	sps->maxSubLayersMinus1 = (int)h264Bits(&br, 3);
	if (sps->maxSubLayersMinus1 > 6)
		return false;
	h264Bit(&br);				/* temporal_id_nesting */
	parseProfileTierLevel(&br, sps->maxSubLayersMinus1, sps);

	uint32_t id = h264UE(&br);
	if (id >= HEVC_MAX_SPS)
		return false;
	sps->id = (int)id;
	sps->chromaFormatIdc = readUnsigned(&br, 3);
	if (sps->chromaFormatIdc > 3)
		return false;
	if (sps->chromaFormatIdc == 3)
		sps->separateColourPlane = (int)h264Bit(&br);
	sps->width = readUnsigned(&br, 16384);
	sps->height = readUnsigned(&br, 16384);
	if (sps->width <= 0 || sps->height <= 0 || sps->width > 16384
		|| sps->height > 16384) {
		return false;
	}
	if (h264Bit(&br)) {			/* conformance_window_flag */
		int subWidth = (sps->chromaFormatIdc == 1 || sps->chromaFormatIdc == 2)
			? 2 : 1;
		int subHeight = sps->chromaFormatIdc == 1 ? 2 : 1;
		sps->confLeft = readUnsigned(&br, sps->width / subWidth) * subWidth;
		sps->confRight = readUnsigned(&br, sps->width / subWidth) * subWidth;
		sps->confTop = readUnsigned(&br, sps->height / subHeight) * subHeight;
		sps->confBottom = readUnsigned(&br, sps->height / subHeight) * subHeight;
		if (sps->confLeft + sps->confRight >= sps->width
			|| sps->confTop + sps->confBottom >= sps->height) {
			return false;
		}
	}
	sps->bitDepthLuma = readUnsigned(&br, 8) + 8;
	sps->bitDepthChroma = readUnsigned(&br, 8) + 8;
	if (sps->bitDepthLuma > 16 || sps->bitDepthChroma > 16)
		return false;
	sps->log2MaxPocLsb = readUnsigned(&br, 12) + 4;
	if (sps->log2MaxPocLsb > 16)
		return false;

	bool orderingForEach = h264Bit(&br);
	for (int i = orderingForEach ? 0 : sps->maxSubLayersMinus1;
			i <= sps->maxSubLayersMinus1; i++) {
		/* Keep what the highest sub-layer says: that is what is decoded. */
		sps->maxDecPicBuffering = readUnsigned(&br, 15) + 1;
		sps->maxNumReorderPics = readUnsigned(&br, sps->maxDecPicBuffering - 1);
		sps->maxLatencyIncreasePlus1 = readUnsigned(&br, INT_MAX);
	}
	if (sps->maxDecPicBuffering > 16 || sps->maxNumReorderPics > 16)
		return false;

	sps->log2MinCbSize = readUnsigned(&br, 3) + 3;
	sps->log2CtbSize = sps->log2MinCbSize + readUnsigned(&br, 3);
	sps->log2MinTbSize = readUnsigned(&br, 3) + 2;
	sps->log2MaxTbSize = sps->log2MinTbSize + readUnsigned(&br, 3);
	if (sps->log2CtbSize < 4 || sps->log2CtbSize > 6 || sps->log2MaxTbSize > 5)
		return false;
	sps->maxTransformHierarchyDepthInter = readUnsigned(&br, 4);
	sps->maxTransformHierarchyDepthIntra = readUnsigned(&br, 4);

	sps->scalingListEnabled = (int)h264Bit(&br);
	setDefaultScaling(&sps->scaling);
	if (sps->scalingListEnabled && h264Bit(&br)) {
		if (!parseScalingListData(&br, &sps->scaling))
			return false;
	}
	sps->ampEnabled = (int)h264Bit(&br);
	sps->saoEnabled = (int)h264Bit(&br);
	sps->pcmEnabled = (int)h264Bit(&br);
	if (sps->pcmEnabled) {
		sps->pcmBitDepthLuma = (int)h264Bits(&br, 4) + 1;
		sps->pcmBitDepthChroma = (int)h264Bits(&br, 4) + 1;
		sps->log2MinPcmCbSize = readUnsigned(&br, 2) + 3;
		sps->log2MaxPcmCbSize = sps->log2MinPcmCbSize + readUnsigned(&br, 2);
		sps->pcmLoopFilterDisabled = (int)h264Bit(&br);
	}

	uint32_t sets = h264UE(&br);
	if (sets > 64)
		return false;
	sps->numShortTermSets = (int)sets;
	for (int i = 0; i < sps->numShortTermSets; i++) {
		if (!parseShortTermSet(&br, sps, i, &sps->shortTerm[i]))
			return false;
	}

	sps->longTermRefsPresent = (int)h264Bit(&br);
	if (sps->longTermRefsPresent) {
		uint32_t count = h264UE(&br);
		if (count > HEVC_MAX_LONG_TERM_SPS)
			return false;
		sps->numLongTermRefsSps = (int)count;
		for (int i = 0; i < sps->numLongTermRefsSps; i++) {
			sps->longTermPocLsbSps[i] = (int)h264Bits(&br, sps->log2MaxPocLsb);
			sps->longTermUsedSps[i] = h264Bit(&br);
		}
	}
	sps->temporalMvpEnabled = (int)h264Bit(&br);
	sps->strongIntraSmoothing = (int)h264Bit(&br);
	if (h264Bit(&br)) {			/* vui_parameters_present_flag */
		if (!parseVui(&br, sps))
			return false;
	}
	if (h264Bit(&br)) {			/* sps_extension_present_flag */
		bool range = h264Bit(&br);
		h264Bit(&br);			/* multilayer */
		h264Bit(&br);			/* 3d */
		h264Bit(&br);			/* scc */
		h264Bits(&br, 4);
		if (range) {
			sps->transformSkipRotation = (int)h264Bit(&br);
			sps->transformSkipContext = (int)h264Bit(&br);
			sps->implicitRdpcm = (int)h264Bit(&br);
			sps->explicitRdpcm = (int)h264Bit(&br);
			sps->extendedPrecision = (int)h264Bit(&br);
			sps->intraSmoothingDisabled = (int)h264Bit(&br);
			sps->highPrecisionOffsets = (int)h264Bit(&br);
			sps->persistentRiceAdaptation = (int)h264Bit(&br);
			sps->cabacBypassAlignment = (int)h264Bit(&br);
		}
	}
	if (overran(&br))
		return false;

	int ctbSize = 1 << sps->log2CtbSize;
	sps->ctbWidth = (sps->width + ctbSize - 1) / ctbSize;
	sps->ctbHeight = (sps->height + ctbSize - 1) / ctbSize;
	sps->valid = true;
	return true;
}

/* ----------------------------------------------------------------- PPS */

/* 7.3.2.3 */
bool
hevcParsePps(const uint8_t *rbsp, size_t size, const HevcParamSets *sets,
	HevcPps *pps)
{
	H264Bits br;
	if (!validHeader(rbsp, size, HEVC_NAL_PPS))
		return false;
	h264BitsInit(&br, rbsp + 2, size - 2);
	memset(pps, 0, sizeof(*pps));

	uint32_t id = h264UE(&br);
	uint32_t spsId = h264UE(&br);
	if (id >= HEVC_MAX_PPS || spsId >= HEVC_MAX_SPS)
		return false;
	pps->id = (int)id;
	pps->spsId = (int)spsId;
	const HevcSps *sps = &sets->sps[spsId];
	if (!sps->valid)
		return false;

	pps->dependentSliceSegmentsEnabled = (int)h264Bit(&br);
	pps->outputFlagPresent = (int)h264Bit(&br);
	pps->numExtraSliceHeaderBits = (int)h264Bits(&br, 3);
	pps->signDataHiding = (int)h264Bit(&br);
	pps->cabacInitPresent = (int)h264Bit(&br);
	pps->numRefIdxL0DefaultActive = readUnsigned(&br, 14) + 1;
	pps->numRefIdxL1DefaultActive = readUnsigned(&br, 14) + 1;
	if (pps->numRefIdxL0DefaultActive > 15 || pps->numRefIdxL1DefaultActive > 15)
		return false;
	pps->initQpMinus26 = h264SE(&br);
	if (pps->initQpMinus26 < -(26 + 6 * (sps->bitDepthLuma - 8)) || pps->initQpMinus26 > 25)
		return false;
	pps->constrainedIntraPred = (int)h264Bit(&br);
	pps->transformSkipEnabled = (int)h264Bit(&br);
	pps->cuQpDeltaEnabled = (int)h264Bit(&br);
	if (pps->cuQpDeltaEnabled)
		pps->diffCuQpDeltaDepth = readUnsigned(&br, sps->log2CtbSize - sps->log2MinCbSize);
	pps->cbQpOffset = h264SE(&br);
	pps->crQpOffset = h264SE(&br);
	if (pps->cbQpOffset < -12 || pps->cbQpOffset > 12 || pps->crQpOffset < -12 || pps->crQpOffset > 12)
		return false;
	pps->sliceChromaQpOffsetsPresent = (int)h264Bit(&br);
	pps->weightedPred = (int)h264Bit(&br);
	pps->weightedBipred = (int)h264Bit(&br);
	pps->transquantBypassEnabled = (int)h264Bit(&br);
	pps->tilesEnabled = (int)h264Bit(&br);
	pps->entropyCodingSync = (int)h264Bit(&br);
	pps->numTileColumns = 1;
	pps->numTileRows = 1;
	pps->uniformSpacing = 1;
	pps->loopFilterAcrossTiles = 1;
	if (pps->tilesEnabled) {
		pps->numTileColumns = readUnsigned(&br, HEVC_MAX_TILE_COLUMNS - 1) + 1;
		pps->numTileRows = readUnsigned(&br, HEVC_MAX_TILE_ROWS - 1) + 1;
		if (pps->numTileColumns > HEVC_MAX_TILE_COLUMNS
			|| pps->numTileRows > HEVC_MAX_TILE_ROWS
			|| pps->numTileColumns > sps->ctbWidth || pps->numTileRows > sps->ctbHeight) {
			return false;
		}
		pps->uniformSpacing = (int)h264Bit(&br);
		if (!pps->uniformSpacing) {
			int width = 0, height = 0;
			for (int i = 0; i < pps->numTileColumns - 1; i++) {
				pps->columnWidth[i] = readUnsigned(&br, sps->ctbWidth - 1) + 1;
				width += pps->columnWidth[i];
			}
			for (int i = 0; i < pps->numTileRows - 1; i++) {
				pps->rowHeight[i] = readUnsigned(&br, sps->ctbHeight - 1) + 1;
				height += pps->rowHeight[i];
			}
			if (width >= sps->ctbWidth || height >= sps->ctbHeight) return false;
		}
		pps->loopFilterAcrossTiles = (int)h264Bit(&br);
	}
	pps->loopFilterAcrossSlices = (int)h264Bit(&br);
	pps->deblockingControlPresent = (int)h264Bit(&br);
	if (pps->deblockingControlPresent) {
		pps->deblockingOverrideEnabled = (int)h264Bit(&br);
		pps->deblockingDisabled = (int)h264Bit(&br);
		if (!pps->deblockingDisabled) {
			pps->betaOffsetDiv2 = h264SE(&br);
			pps->tcOffsetDiv2 = h264SE(&br);
			if (pps->betaOffsetDiv2 < -6 || pps->betaOffsetDiv2 > 6
				|| pps->tcOffsetDiv2 < -6 || pps->tcOffsetDiv2 > 6) return false;
		}
	}
	pps->scalingListPresent = (int)h264Bit(&br);
	if (pps->scalingListPresent) {
		if (!parseScalingListData(&br, &pps->scaling))
			return false;
	}
	pps->listsModificationPresent = (int)h264Bit(&br);
	pps->log2ParallelMergeLevel = readUnsigned(&br, sps->log2CtbSize - 2) + 2;
	pps->sliceHeaderExtensionPresent = (int)h264Bit(&br);
	if (h264Bit(&br)) {			/* pps_extension_present_flag */
		bool range = h264Bit(&br);
		h264Bit(&br);
		h264Bit(&br);
		h264Bit(&br);
		h264Bits(&br, 4);
		if (range) {
			if (pps->transformSkipEnabled)
				pps->log2MaxTransformSkipSizeMinus2 = readUnsigned(&br, 3);
			pps->crossComponentPrediction = (int)h264Bit(&br);
			pps->chromaQpOffsetListEnabled = (int)h264Bit(&br);
			if (pps->chromaQpOffsetListEnabled) {
				pps->diffCuChromaQpOffsetDepth = readUnsigned(&br, sps->log2CtbSize - sps->log2MinCbSize);
				pps->chromaQpOffsetListLen = readUnsigned(&br, 5) + 1;
				if (pps->chromaQpOffsetListLen > 6)
					return false;
				for (int i = 0; i < pps->chromaQpOffsetListLen; i++) {
					pps->cbQpOffsetList[i] = h264SE(&br);
					pps->crQpOffsetList[i] = h264SE(&br);
				}
			}
			pps->log2SaoOffsetScaleLuma = readUnsigned(&br, 6);
			pps->log2SaoOffsetScaleChroma = readUnsigned(&br, 6);
		}
	}
	if (overran(&br))
		return false;
	pps->valid = true;
	return true;
}

void
hevcTileSizes(const HevcSps *sps, const HevcPps *pps, int *columnWidth,
	int *rowHeight)
{
	int columns = pps->numTileColumns, rows = pps->numTileRows;
	if (pps->uniformSpacing) {
		for (int i = 0; i < columns; i++) {
			columnWidth[i] = ((i + 1) * sps->ctbWidth) / columns
				- (i * sps->ctbWidth) / columns;
		}
		for (int i = 0; i < rows; i++) {
			rowHeight[i] = ((i + 1) * sps->ctbHeight) / rows
				- (i * sps->ctbHeight) / rows;
		}
		return;
	}
	int used = 0;
	for (int i = 0; i < columns - 1; i++) {
		columnWidth[i] = pps->columnWidth[i];
		used += columnWidth[i];
	}
	columnWidth[columns - 1] = sps->ctbWidth - used;
	used = 0;
	for (int i = 0; i < rows - 1; i++) {
		rowHeight[i] = pps->rowHeight[i];
		used += rowHeight[i];
	}
	rowHeight[rows - 1] = sps->ctbHeight - used;
}

/* -------------------------------------------------------- slice header */

/* 7.3.6.1, up to slice_temporal_mvp_enabled_flag. */
bool
hevcParseSliceHeader(const uint8_t *rbsp, size_t size,
	const HevcParamSets *sets, HevcSlice *slice)
{
	H264Bits br;
	if (rbsp == NULL || size < 3 || !validHeader(rbsp, size, (rbsp[0] >> 1) & 63)
		|| !hevcIsVcl((rbsp[0] >> 1) & 63))
		return false;
	memset(slice, 0, sizeof(*slice));
	slice->nalType = (rbsp[0] >> 1) & 0x3f;
	slice->temporalId = (rbsp[1] & 7) - 1;
	h264BitsInit(&br, rbsp + 2, size - 2);

	slice->firstSliceInPicture = (int)h264Bit(&br);
	if (hevcIsIrap(slice->nalType))
		slice->noOutputOfPriorPics = (int)h264Bit(&br);
	uint32_t ppsId = h264UE(&br);
	if (ppsId >= HEVC_MAX_PPS || !sets->pps[ppsId].valid)
		return false;
	slice->ppsId = (int)ppsId;
	const HevcPps *pps = &sets->pps[ppsId];
	const HevcSps *sps = &sets->sps[pps->spsId];
	if (!sps->valid)
		return false;

	if (!slice->firstSliceInPicture) {
		if (pps->dependentSliceSegmentsEnabled)
			slice->dependentSliceSegment = (int)h264Bit(&br);
		uint32_t address = h264Bits(&br, ceilLog2(sps->ctbWidth * sps->ctbHeight));
		if (address == 0 || address >= (uint32_t)(sps->ctbWidth * sps->ctbHeight)) return false;
	}
	if (slice->dependentSliceSegment)
		return !overran(&br);

	h264Bits(&br, pps->numExtraSliceHeaderBits);
	uint32_t sliceType = h264UE(&br);
	if (sliceType > 2)
		return false;
	slice->sliceType = (int)sliceType;
	size_t skipStart = bitPosition(&br);

	slice->picOutputFlag = 1;
	if (pps->outputFlagPresent)
		slice->picOutputFlag = (int)h264Bit(&br);
	if (sps->separateColourPlane)
		h264Bits(&br, 2);
	if (!hevcIsIdr(slice->nalType)) {
		slice->pocLsb = (int)h264Bits(&br, sps->log2MaxPocLsb);
		slice->shortTermSetSpsFlag = (int)h264Bit(&br);
		if (!slice->shortTermSetSpsFlag) {
			size_t before = bitPosition(&br);
			if (!parseShortTermSet(&br, sps, sps->numShortTermSets,
					&slice->shortTerm)) {
				return false;
			}
			slice->shortTermSetBits = (int)(bitPosition(&br) - before);
		} else {
			if (sps->numShortTermSets == 0)
				return false;
			int index = 0;
			if (sps->numShortTermSets > 1)
				index = (int)h264Bits(&br, ceilLog2(sps->numShortTermSets));
			if (index >= sps->numShortTermSets)
				return false;
			slice->shortTermSetIndex = index;
			slice->shortTerm = sps->shortTerm[index];
		}
		if (sps->longTermRefsPresent) {
			uint32_t fromSps = 0;
			if (sps->numLongTermRefsSps > 0)
				fromSps = h264UE(&br);
			uint32_t own = h264UE(&br);
			if (fromSps > (uint32_t)sps->numLongTermRefsSps
				|| own > HEVC_MAX_LONG_TERM || fromSps + own > HEVC_MAX_LONG_TERM) {
				return false;
			}
			slice->numLongTerm = (int)(fromSps + own);
			for (int i = 0; i < slice->numLongTerm; i++) {
				if (i < (int)fromSps) {
					int index = 0;
					if (sps->numLongTermRefsSps > 1)
						index = (int)h264Bits(&br,
							ceilLog2(sps->numLongTermRefsSps));
					if (index >= sps->numLongTermRefsSps) return false;
					slice->longTermPocLsb[i] = sps->longTermPocLsbSps[index];
					slice->longTermUsed[i] = sps->longTermUsedSps[index];
				} else {
					slice->longTermPocLsb[i] = (int)h264Bits(&br,
						sps->log2MaxPocLsb);
					slice->longTermUsed[i] = h264Bit(&br);
				}
				slice->longTermMsbPresent[i] = h264Bit(&br);
				int64_t cycle = 0;
				if (slice->longTermMsbPresent[i])
					cycle = readUnsigned(&br, INT_MAX);
				/* 7-52: the cycles add up within each group. */
				if (i != 0 && i != (int)fromSps)
					cycle += slice->longTermMsbCycle[i - 1];
				if (cycle > INT_MAX) return false;
				slice->longTermMsbCycle[i] = cycle;
			}
		}
	}
	slice->skipBits = (int)(bitPosition(&br) - skipStart);
	return !overran(&br);
}

int
hevcNumPicTotalCurr(const HevcSlice *slice)
{
	if (slice->sliceType == 2)
		return 0;
	int total = 0;
	for (int i = 0; i < slice->shortTerm.numNegative; i++)
		total += slice->shortTerm.used[0][i];
	for (int i = 0; i < slice->shortTerm.numPositive; i++)
		total += slice->shortTerm.used[1][i];
	for (int i = 0; i < slice->numLongTerm; i++)
		total += slice->longTermUsed[i];
	return total;
}
