/*
 * Format.h - human-readable formatting helpers
 *
 * GetSizeString / GetUsedMax / GetUsedMaxSize mirror the helpers used by
 * HaikuUtils' SystemManager so finite resources (used vs total) are presented
 * the same way the rest of the system tools present them.
 */

#pragma once

#include <SupportDefs.h>

class BString;

// "1.50 GB" style size, binary (1024) prefixes.
void GetSizeString(BString& str, uint64 size);

// "used/max (NN%)" for plain counts.
void GetUsedMax(BString& str, uint64 used, uint64 max);

// "used/max (NN%)" with both operands formatted as sizes.
void GetUsedMaxSize(BString& str, uint64 used, uint64 max);
