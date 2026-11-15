/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_BITS_H
#define CEDAR_BITS_H

/*	Annex B splitting, emulation-prevention removal and an RBSP bit reader
	for the H.264 and HEVC parsers. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the next NAL unit in an Annex B buffer. *pos is the scan position
 * (start with 0). Returns 1 and sets nal and size (start code excluded,
 * trailing zero bytes trimmed), or 0 at the end. */
int annexb_next(const uint8_t *buf, size_t len, size_t *pos,
	const uint8_t **nal, size_t *size);

/* An RBSP: the NAL with emulation-prevention bytes removed. epb[] holds,
 * for each removed byte, its offset in the raw NAL, so offsets can be
 * mapped back. */
typedef struct {
	uint8_t		*data;
	size_t		size;
	size_t		*epb;
	int		epbCount;
} Rbsp;

int rbsp_from_nal(Rbsp *rbsp, const uint8_t *nal, size_t size);
void rbsp_free(Rbsp *rbsp);
/* RBSP byte offset -> raw NAL byte offset */
size_t rbsp_to_raw(const Rbsp *rbsp, size_t rbspOffset);

typedef struct {
	const uint8_t	*data;
	size_t		size;		/* bytes */
	size_t		pos;		/* bits */
	int		overrun;
} BitReader;

void br_init(BitReader *br, const uint8_t *data, size_t size);
uint32_t br_u(BitReader *br, int bits);	/* bits <= 32 */
uint32_t br_ue(BitReader *br);
int32_t br_se(BitReader *br);
static inline int br_flag(BitReader *br) { return (int)br_u(br, 1); }
static inline size_t br_pos(const BitReader *br) { return br->pos; }
void br_skip(BitReader *br, size_t bits);
/* more_rbsp_data(): true if there is anything but the stop bit left */
int br_more_rbsp_data(const BitReader *br);
int br_byte_aligned(const BitReader *br);

#ifdef __cplusplus
}
#endif

#endif	/* CEDAR_BITS_H */
