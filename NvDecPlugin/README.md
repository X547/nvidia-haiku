# NvDecPlugin

A Haiku media decoder add-on that decodes H.264, H.265 and VP9 on the GPU's
NVDEC engine, driven directly through NVIDIA's resource manager: no Vulkan,
no NVK. Any program that plays video through the Media Kit gets it.

This is a port of air/OS's `nvdec` media add-on
([airos-works/haiku](https://github.com/airos-works/haiku),
`src/add-ons/media/plugins/nvdec`, commit `12ad0480`), which was written for
Pascal GPUs and air/OS's `nvidia_rm` driver. Its own README, with the details
of the engine and of what was measured on a GTX 1070, is
[README.airos.md](README.airos.md).

## What changed in the port

- `src/NvdecEngine.cpp` replaces air/OS's `nvdec_engine.c`. It implements the
  same interface (`nvdec_engine.h`) on this repository's `nvrm_cpp_sdk` and
  NvRmDemos' channel helpers (`nvrm-helpers-cpp`):
  - the channel is submitted through the usermode doorbell (Turing and
    later), not by writing `GPPut` as on Pascal;
  - the decoder class is the newest NVDEC class the GPU lists (NVC4B0 to
    NVCFB0), allocated with `NV_BSP_ALLOCATION_PARAMETERS`;
  - after the engine's completion semaphore, the channel waits for it and
    raises a non-stall interrupt, so the waiting thread sleeps on an OS event
    instead of polling every 100 µs (the engine's own `SEMAPHORE_D`
    `AWAKEN_ENABLE` interrupt does not wake it);
  - a channel killed by resman is reported with its reason instead of a
    timeout;
  - from NVC7B0 (Ampere GA10x) on, every method list carries
    `SET_INTRA_TOP_BUF_OFFSET` (0x4C8) with a 1 MiB buffer: those engines
    keep the line above the current block row there for H.265 and VP9 (and
    AV1), and fault on address 0 without it. Pascal's class has no such
    method, so air/OS's code never sent it. Daniel Almeida's NVK H.265 branch
    (`nvk-vulkan-video-h265` in his Mesa fork) sends it with 64 KiB;
  - while pictures are being decoded the engine asks resman for the highest
    P-state (`NV2080_CTRL_CMD_PERF_BOOST`, two seconds, renewed at most once a
    second; cleared when the decoder closes). Decoding a picture every few
    tens of milliseconds otherwise leaves the GPU in its idle P-state with the
    video clock at 555 MHz, and a 4K H.264 picture takes 19 ms instead of 8.8
    on an RTX A2000. `NVDEC_BOOST=0` turns it off. Resman accepts neither the
    one-level boost nor the video stream events
    (`NV2080_CTRL_CMD_PERF_NOTIFY_VIDEOEVENT`) to the same effect on GSP
    firmware.
- `NVDecPlugin.cpp` offers H.265 and VP9 to the media kit as well as H.264
  (see below), and exports `instantiate_plugin()` explicitly (the add-on is
  built with hidden visibility).
- Everything else in `src/` is air/OS's code unchanged. `src/cedar/` holds the
  three files of air/OS's `sunxi_cedar` add-on that the VP9 decoder shares,
  and `src/nvidia/video/nvdec_drv.h` is NVIDIA's header as Mesa carries it.
- `tools/nvdecstream.c` is air/OS's stream test
  (`docs/x399-workstation/tests`): it decodes an Annex B H.264 file and
  writes I420 that can be compared byte for byte with
  `ffmpeg -i stream.h264 -pix_fmt yuv420p -f rawvideo`.
  With `NVDEC_NV12=1` it writes NV12 as the add-on does.
- `tools/mediadecode.cpp` is air/OS's Media Kit test: it opens a file with
  `BMediaFile`, decodes its video track and says which decoder did it. It
  uses the add-on installed where the media kit looks for add-ons.

## Codecs

Measured on an RTX A2000 (GA106, NVC7B0) with the Vulkan CTS clips:

| Codec | Result |
| --- | --- |
| H.264 | byte for byte as ffmpeg decodes, up to 3840x2160 (`nvdecstream`) |
| H.265 | every picture of the five CTS streams decodes, up to 3840x2160, also from MP4 and MKV; the output has not yet been compared with ffmpeg's byte for byte |
| VP9 | every picture of the eight-bit CTS streams decodes, up to 3980x2160, also from WebM; ten-bit streams are refused; not yet compared byte for byte |

`hevc-itu-slist-b.h265` gives 62 pictures through the media kit where
ffmpeg's add-on gives 65.

All three are offered to the media kit. `NVDEC_CODECS` lists the codecs
to offer instead, by name (`h264`, `hevc` or `h265`, `vp9`), separated by
commas, for example `NVDEC_CODECS=h264`. It is read in the process that plays
the video, when the media kit first looks at its add-ons. Haiku gives a format
to one decoder and does not fall back, so a stream of an offered codec that
the add-on refuses (H.265 beyond Main 10 4:2:0, VP9 beyond profile 0) does not
play.

The arrangement of decoded pictures in memory (`nvdecTileOffset`) and the
NVDEC method offsets are the same on Ampere as on Pascal (the H.264 output
matches ffmpeg's exactly, and the class headers agree).

## Where the time goes

Through the media kit at 3840x2160 on an RTX A2000, decoding takes 8.8 ms a
picture with the clock boost (19 ms without). Picture setup on the CPU takes
0.25 ms and the wait for the decoder costs about 0.2 ms more than the
decoding itself. Converting to RGB32 on the CPU, untiling as it goes, takes
longer than decoding.

Settings, read from the environment of the program playing the video:

| Variable | Effect |
| --- | --- |
| `NVDEC_CODECS` | the codecs offered to the media kit (see above) |
| `NVDEC_BOOST=0` | no clock boost while decoding |
| `NVDEC_SURFACES_IN_VRAM=1` | decoded pictures in video memory (air/OS's switch) |
| `NVDEC_CONVERT_SINGLE=1` | convert on one thread |
| `NVDEC_TIMING=1` | print where the time went every sixty pictures |

## Building

After `NvRmDemos` is installed:

```
meson setup build.x86_64/NvDecPlugin source/NvDecPlugin \
  -Dpkg_config_path=$PWD/install.x86_64/develop/lib/pkgconfig \
  -Dprefix=$PWD/install.x86_64
ninja -C build.x86_64/NvDecPlugin install
```

The add-on is installed to `add-ons/media/plugins/nvdec.so`. Haiku picks one
decoder per format, so once installed where the media kit looks for add-ons
it takes the codecs it offers over from the ffmpeg add-on on every machine
that has `/dev/nvidiactl`.

## Licence

air/OS's code is under the MIT licence (see `LICENSE.airos`). The VP9 tables
(`src/nvdec_vp9_tables.c`, `src/cedar/vp9_tables.c`) come from libvpx and
keep its BSD notice. `nvdec_drv.h` is NVIDIA's, under the MIT licence stated
in the file.
