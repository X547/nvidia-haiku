/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "bits.h"

#include <stdlib.h>
#include <string.h>


int
annexb_next(const uint8_t *buf, size_t len, size_t *pos,
	const uint8_t **nal, size_t *size)
{
	size_t i = *pos;

	/* find a 00 00 01 start code */
	for (;;) {
		if (i + 3 > len)
			return 0;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
			break;
		i++;
	}
	i += 3;
	size_t start = i;

	/* the unit ends at the next 00 00 00 or 00 00 01 */
	while (i + 3 <= len) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] <= 1)
			break;
		i++;
	}
	if (i + 3 > len)
		i = len;
	size_t end = i;
	while (end > start && buf[end - 1] == 0)
		end--;

	*nal = buf + start;
	*size = end - start;
	*pos = i;
	return 1;
}


int
rbsp_from_nal(Rbsp *rbsp, const uint8_t *nal, size_t size)
{
	rbsp->data = malloc(size + 8);
	/* at most one prevention byte in three */
	rbsp->epb = malloc((size / 3 + 1) * sizeof(size_t));
	if (rbsp->data == NULL || rbsp->epb == NULL) {
		free(rbsp->data);
		free(rbsp->epb);
		rbsp->data = NULL;
		rbsp->epb = NULL;
		return -1;
	}
	rbsp->epbCount = 0;

	size_t out = 0;
	int zeros = 0;
	for (size_t i = 0; i < size; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			/* 00 00 03: drop the 03 */
			rbsp->epb[rbsp->epbCount++] = i;
			zeros = 0;
			continue;
		}
		rbsp->data[out++] = nal[i];
		zeros = nal[i] == 0 ? zeros + 1 : 0;
	}
	memset(rbsp->data + out, 0, 8);
	rbsp->size = out;
	return 0;
}


void
rbsp_free(Rbsp *rbsp)
{
	free(rbsp->data);
	free(rbsp->epb);
	rbsp->data = NULL;
	rbsp->epb = NULL;
}


size_t
rbsp_to_raw(const Rbsp *rbsp, size_t rbspOffset)
{
	size_t raw = rbspOffset;
	for (int i = 0; i < rbsp->epbCount; i++) {
		if (rbsp->epb[i] <= raw)
			raw++;
		else
			break;
	}
	return raw;
}


void
br_init(BitReader *br, const uint8_t *data, size_t size)
{
	br->data = data;
	br->size = size;
	br->pos = 0;
	br->overrun = 0;
}


uint32_t
br_u(BitReader *br, int bits)
{
	uint32_t value = 0;
	for (int i = 0; i < bits; i++) {
		size_t byte = br->pos >> 3;
		uint32_t bit = 0;
		if (byte < br->size)
			bit = (br->data[byte] >> (7 - (br->pos & 7))) & 1;
		else
			br->overrun = 1;
		value = (value << 1) | bit;
		br->pos++;
	}
	return value;
}


uint32_t
br_ue(BitReader *br)
{
	int leadingZeros = 0;
	while (br_u(br, 1) == 0) {
		if (++leadingZeros > 31 || br->overrun) {
			br->overrun = 1;
			return 0;
		}
	}
	if (leadingZeros == 0)
		return 0;
	return ((1u << leadingZeros) - 1) + br_u(br, leadingZeros);
}


int32_t
br_se(BitReader *br)
{
	uint32_t k = br_ue(br);
	if (k & 1)
		return (int32_t)((k + 1) / 2);
	return -(int32_t)(k / 2);
}


void
br_skip(BitReader *br, size_t bits)
{
	br->pos += bits;
	if ((br->pos + 7) / 8 > br->size)
		br->overrun = 1;
}


int
br_more_rbsp_data(const BitReader *br)
{
	if (br->pos >= br->size * 8)
		return 0;
	/* find the last 1 bit in the buffer (the stop bit) */
	size_t last = br->size;
	while (last > 0 && br->data[last - 1] == 0)
		last--;
	if (last == 0)
		return 0;
	uint8_t b = br->data[last - 1];
	int trailing = 0;
	while (((b >> trailing) & 1) == 0)
		trailing++;
	size_t stopBit = (last - 1) * 8 + (7 - trailing);
	return br->pos < stopBit;
}


int
br_byte_aligned(const BitReader *br)
{
	return (br->pos & 7) == 0;
}
