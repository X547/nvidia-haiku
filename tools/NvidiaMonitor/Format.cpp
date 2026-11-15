/*
 * Format.cpp - see Format.h
 */

#include "Format.h"

#include <String.h>


void GetSizeString(BString& str, uint64 size)
{
	const char* prefixes[] = {"", "K", "M", "G", "T"};
	unsigned pow = 0;
	int fracSize = 0;
	while ((size >= 1024) && (pow < sizeof(prefixes) / sizeof(prefixes[0]) - 1)) {
		fracSize = size * 100 / 1024 % 100;
		size /= 1024;
		pow++;
	}
	str.SetToFormat("%" B_PRIu64 ".%02d %sB", size, fracSize, prefixes[pow]);
}


void GetUsedMax(BString& str, uint64 used, uint64 max)
{
	int32 ratio = 100;
	if (max > 0)
		ratio = int32(double(used) / double(max) * 100.0);
	str.SetToFormat("%" B_PRIu64 "/%" B_PRIu64 " (%" B_PRId32 "%%)", used, max, ratio);
}


void GetUsedMaxSize(BString& str, uint64 used, uint64 max)
{
	BString str2;
	int32 ratio = 100;
	if (max > 0)
		ratio = int32(double(used) / double(max) * 100.0);
	GetSizeString(str, used);
	str += "/";
	GetSizeString(str2, max);
	str += str2;
	str2.SetToFormat(" (%" B_PRId32 "%%)", ratio);
	str += str2;
}
