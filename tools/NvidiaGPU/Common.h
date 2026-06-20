/*
 * Common.h - Shared definitions for NvidiaGPU app
 */

#ifndef NVIDIAGPU_COMMON_H
#define NVIDIAGPU_COMMON_H

#define APP_SIGNATURE "application/x-vnd.NvidiaGPU"

// Window modes
enum {
	NORMAL_WINDOW_MODE = 0,
	DESKBAR_MODE
};

// Message codes
#define MSG_PULSE              'puls'
#define MSG_NORMAL_MODE        'norm'
#define MSG_DESKBAR_MODE       'dskb'
#define MSG_ABOUT              'abou'
#define MSG_QUIT               'quit'
#define MSG_REPLICANT_PULSE    'rpls'

// Layout constants
#define VIEW_WIDTH             290
#define VIEW_HEIGHT            95

#define PROGRESS_BAR_HEIGHT    16
#define PROGRESS_BAR_WIDTH     150
#define LABEL_WIDTH            70

// Colors - NVIDIA green (#76B900)
#define GPU_LOAD_COLOR_R       0x76
#define GPU_LOAD_COLOR_G       0xB9
#define GPU_LOAD_COLOR_B       0x00

#define MEM_LOAD_COLOR_R       0x20
#define MEM_LOAD_COLOR_G       0x80
#define MEM_LOAD_COLOR_B       0xC0

#define TEMP_WARN_THRESHOLD    70
#define TEMP_CRIT_THRESHOLD    85

#endif // NVIDIAGPU_COMMON_H
