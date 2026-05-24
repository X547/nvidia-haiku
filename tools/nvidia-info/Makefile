# nvidia-info Makefile for Haiku

CXX = g++
CXXFLAGS = -Wall -O2 -std=c++17

# Paths
BASE = ../../source
BUILD = ../../build.x86_64
INSTALL = ../../install.x86_64

NVRM_SDK = $(BASE)/nvrm_sdk/include
NVRM_SDK_ROOT = $(BASE)/nvrm_sdk
NVRM_CPP_SDK = $(BASE)/nvrm_cpp_sdk
OPEN_GPU = $(BASE)/open-gpu-kernel-modules/src/common/sdk/nvidia/inc
NV_UNIX = $(BASE)/open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include
NV_COMMON = $(BASE)/open-gpu-kernel-modules/src/common/inc
NV_KERNEL = $(BASE)/open-gpu-kernel-modules/kernel-open/common/inc

INCLUDES = -I$(NVRM_SDK) -I$(NVRM_SDK_ROOT) -I$(NVRM_CPP_SDK) -I$(OPEN_GPU) -I$(OPEN_GPU)/ctrl \
           -I$(NV_UNIX) -I$(NV_COMMON) -I$(NV_KERNEL)

# No extra libraries needed for basic info
LDFLAGS =

TARGET = nvidia-info
SOURCES = nvidia-info.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

install: $(TARGET)
	cp $(TARGET) /boot/system/non-packaged/bin/

clean:
	rm -f $(TARGET)

.PHONY: all install clean
