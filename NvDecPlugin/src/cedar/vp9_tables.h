/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_VP9_TABLES_H
#define CEDAR_VP9_TABLES_H

/*	VP9's constant tables (vp9_tables.c, from libvpx under its BSD
	licence). */

#include "vp9_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint16_t kVp9DcQ8[256];
extern const uint16_t kVp9AcQ8[256];
extern const uint16_t kVp9DcQ10[256];
extern const uint16_t kVp9AcQ10[256];
extern const uint8_t kVp9InverseMap[254];

#ifdef __cplusplus
}
#endif

#endif	/* CEDAR_VP9_TABLES_H */
