# Compute shaders

The demos launch these shaders through the compute class directly
([NvCompute.h](../NvCompute.h)), so they need the machine code, not SPIR-V.
`*Shader.h` holds that code, compiled for SM 86 (GA10x: RTX 30x0, RTX A2000 and
others) by NVK and taken out of it by `NvShaderDump`, which is built when the
Vulkan loader is found. To regenerate a header on Haiku after changing a
shader, with NVK installed:

```
glslangValidator --target-env vulkan1.3 -o /tmp/nv12_to_rgb32.spv nv12_to_rgb32.comp
build.x86_64/NvRmDemos/NvShaderDump /tmp/nv12_to_rgb32.spv Nv12ToRgb32 Nv12ToRgb32Shader.h
```

A shader may take its parameters only as push constants (buffers by device
address), and may not need local (scratch) memory or constant data;
`NvShaderDump` refuses anything else. A GPU of another shader model needs the
headers made on it: `NvComputeProgram` refuses code for a different one.
