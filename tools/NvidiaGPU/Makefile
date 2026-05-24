# NvidiaGPU Makefile for Haiku

NAME = NvidiaGPU
TYPE = APP
SRCS = \
	NvidiaGPUApp.cpp \
	NvidiaGPUWindow.cpp \
	NvidiaGPUView.cpp \
	GPUProgressBar.cpp

RDEFS = NvidiaGPU.rdef

LIBS = be localestub $(STDCPPLIBS)

SYSTEM_INCLUDE_PATHS =
LOCAL_INCLUDE_PATHS = .

OPTIMIZE := FULL

WARNINGS = ALL

SYMBOLS := FALSE

DEBUGGER := FALSE

COMPILER_FLAGS =
LINKER_FLAGS =

APP_VERSION :=

DRIVER_PATH =

# Include the Makefile-Engine
DEVEL_DIRECTORY := /boot/system/develop
include $(DEVEL_DIRECTORY)/etc/makefile-engine
