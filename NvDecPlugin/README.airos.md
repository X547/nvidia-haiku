# The graphics card's video decoder, as a media add-on

The GeForce cards this fork runs on have a video engine - NVDEC - that decodes
H.264, H.265 and VP9 without the processor doing any of the arithmetic. This
add-on offers H.264 to Haiku's media kit, so that any program that plays video
gets it: the file is opened as usual, and the pictures come back from the card.
H.265 and VP9 are there for programs that ask for them (see below).

| | |
| --- | --- |
| `nvdec_engine.c` | The engine itself: a resman client, an address space, buffers both sides can see, and a channel on the decoder with an NVC2B0 object on it. Also where the card's arrangement of pixels in memory is written down. |
| `h264_parse.c` | Sequence and picture parameter sets, and as much of a slice header as says which picture a slice belongs to. Nothing here touches picture data: the engine parses every entropy-coded bit itself. |
| `nvdec_h264.c` | The bookkeeping the engine does not do - which pictures are still needed as references, what order they are shown in, which surface each one is in. |
| `hevc_parse.c` | The same for H.265: parameter sets, and slice headers as far as the reference picture sets, whose length in bits the engine is told so it can step over them. |
| `nvdec_hevc.c` | H.265's reference picture sets, picture order and output ("bumping") process, and the picture setup. |
| `nvdec_vp9.c` | VP9: the eight reference slots, the engine's probability table and symbol counts, the segment map and motion vector buffers, and the frame setup. The headers, the probabilities and their adaptation are sunxi_cedar's parser (`../sunxi_cedar/vp9_parse.c`), which this shares. |
| `nvdec_vp9_tables.c` | VP9's key frame probabilities, from libvpx (BSD licence). |
| `nvdec_convert.c` | Out of the card's arrangement and into pixels, in bands, across as many processors as there are. |
| `NVDecPlugin.cpp` | The media kit's side of it. |

## What it does and does not decode

Eight bit 4:2:0 H.264, progressive, up to what the engine allows - 4096 wide on
this generation. Field pictures, 4:2:2, 4:4:4 and more than eight bits a sample
are refused rather than decoded wrongly.

H.265 Main and Main 10, 4:2:0, up to 8192 by 8192. Ten-bit pictures come out
as sixteen-bit samples with the value in the top bits; a caller can have them
as they are by asking for the colour space `'P010'` (`NVDEC_COLOR_SPACE_P010`
in `NVDecPlugin.h`), or as eight-bit YCbCr422 or RGB32 like H.264.

Both can also be had as NV12, by asking for the colour space `'NV12'`
(`NVDEC_COLOR_SPACE_NV12`): the luma plane, then Cb and Cr in pairs on half as
many lines, both `bytes_per_row` (the width) a line - what the engine writes,
untiled, with no colour conversion, and what a graphics card takes as a video
texture. Ten-bit pictures keep their top eight bits.

VP9 profile 0 - eight-bit 4:2:0 - from 65x65 up to 4096x2304 on Pascal. The
engine reads the tile data; the uncompressed and compressed headers are parsed
here, the probabilities the engine decodes with are written for it, and after
each frame its symbol counts are read back for the backward adaptation, as
libvpx does it. Superframes are split; a chunk shows at most one frame, the
last one shown (as libvpx: a spatial SVC superframe shows its top layer).
The GTX 1070's GP104 does not decode ten-bit VP9 (profile 2): asked to, it
keeps eight bits of every sample, so such streams are refused at their first
frame, as are profiles 1 and 3 and frames 64 pixels or less wide or tall (the
engine reports error 3 for those, and never finishes the inter frames of one 8
or 10 pixels across). `NVDEC_VP9_TEN_BIT=1` lets ten-bit streams through for
a chip said to decode them (GP102, GP107, Turing); that is not measured.

Haiku picks one decoder for a format, by add-on directory, so this add-on takes
H.264 away from libavcodec wherever it is installed and `/dev/nvidiactl`
exists (on a machine without nvidia_rm or an NVIDIA card it offers nothing, so
the air/OS x86_64 image can carry it everywhere). A stream it refuses will
not fall back: it will not play. That is the reason for refusing narrowly and
loudly rather than trying - and the reason H.265 and VP9 are not offered to
the media kit at all: a program that wants them instantiates the decoder
itself, sets it up with an H.265 or VP9 format, and keeps libavcodec for what
it refuses. airTime and Summit do. `NewDecoder()` makes decoders for indexes 0
to 2 - H.264, H.265, VP9; any of them takes any of the three codecs - and
returns NULL for the rest, so that a program can tell this add-on from one
before VP9, which made a decoder for every index.

## Building it

It is not built with the rest of the tree, because it needs NVIDIA's resource
manager headers, which live in the NVK tree (airos-works/mesa-nvk, branch
`airos-nvk-r2`), and it compiles three of sunxi_cedar's files (VP9's parser,
its tables and the bit reader). `build-cross.sh` cross-builds it from a Haiku build
directory and that tree (the air/OS CI does, for the x86_64 image);
`docs/x399-workstation/tools/build-nvdec-plugin.sh` builds it on the
workstation and installs it into the user's non-packaged add-ons.

## What was measured

Thirteen H.264 streams and 120 frames of 1080p Big Buck Bunny decode byte for
byte identically to ffmpeg's own decoder. 1080p decodes at 232 pictures a
second - 4.3 ms each - and with the conversion to pixels, a whole 1080p film
plays at 148 pictures a second through the media kit. `tests/nvdecstream.c`
does the comparison; `tests/mediadecode.cpp` goes through the media kit and
says which decoder answered.

For H.265, eighteen streams made with x265 to try one feature each - B
pyramids, open GOPs, ten bits, transform skip, default and custom scaling
lists, slices, wavefronts, 16 and 32 pixel CTUs, weighted prediction, lossless,
no deblocking or SAO, six references, a cropped 1080p - and 240 pictures of a
4K HDR10 film all decode byte for byte as ffmpeg does
(`tests/nvdechevcstream.c`). A 4K ten-bit picture takes 8 ms in the engine and
6 ms to copy out as P010: the film plays at its 24 pictures a second with time
to spare.

Decoding that starts at a CRA picture - after a seek into an open GOP, in a
new decoder or after `SeekedTo()` - or that follows an end of sequence leaves
out the CRA picture's leading RASL pictures, which refer to pictures from
before it (H.265 8.1.3), as libavcodec does: the first picture out is the CRA
picture. Started at each CRA picture of four x265 streams, the pictures match
libavcodec's decode of the same cut byte for byte.

For VP9, 35 of libvpx's test vectors - sizes from 155x115 to 1920x1080 and
odd ones, resizing with scaled references, intra-only frames,
show_existing_frame, superframes, spatial SVC, tiles of 1 to 4 columns and
rows, frame-parallel streams, segmentation with temporal maps, loop filter
deltas, delta-q, lossless, bilinear filtering - and 2,340 frames of five YouTube streams (itags 248,
271, 303, 308 and 315, 818p24 to 2160p60) decode byte for byte as
libavcodec does (`tests/nvdecvp9stream.c` against `tests/nvdecvp9ref.py`),
save one pixel off by one in each of the last five frames of the 66x66
vector. Through the add-on's NV12 output, 300 pictures each of a 1080p60 and
an 818p24 YouTube stream match libavcodec's NV12 MD5s. The engine takes 2.4
ms for a 1080p60 frame and 7.9 ms for a 2160p60 one. The filter type is the
one thing not as nvdec_drv.h suggests: the engine wants it as the header
codes it (smooth 0, regular 1), not in libvpx's order.

Two things about H.265 are not as the header that describes the engine says:
the maximum transform skip size is sent as its log2, not log2 minus two, and
the scaling lists go column by column, which the Tegra driver's raster order
only gets away with for the default lists, which are symmetric.

Four things about the engine are not in the header that describes it, and each
cost a measurement to find. They are in the commit messages and in the comments
where they matter: the arrangement of pixels, the field markings that decide
whether a reference is a reference, the end of stream marker the bitstream
length has to count, and the reference table place a picture must keep.

## Parser regression checks

`jam h264_parse_test` builds a CPU-only test; it needs no NVIDIA headers or
GPU. It checks truncated parameter sets, unsigned-code limits, oversized
IDs, unterminated and excessive reference-list modifications, and MMCO
limits, then tries 25,000 reproducible malformed inputs. Optional arguments
are Annex B H.264 files; `--hevc` switches subsequent files to HEVC. The
stream checks parse every parameter set and slice header, without decoding
pixels. SPS dimensions are bounded to 8192 pixels by this parser; that
does not expand the engine's hardware capabilities.

The shared bit reader reports sticky errors, including truncation and a
code with 32 leading zero bits. H.264 parameter-set IDs and counts are
checked before indexing or arithmetic, complete parameter sets require
their RBSP stop bit, and overlong lists fail instead of silently dropping
entries. HEVC propagates the same reader failure. These checks do not
replace codec capability checks or validation at the kernel boundary.

On 2026-10-10 the test passed under host AddressSanitizer and
UndefinedBehaviorSanitizer and in x86_64 Haiku QEMU: 23 H.264 streams,
1,214 slice headers, and two generated HEVC Main/Main 10 streams with
24 slice headers each. The full NVDEC add-on cross-build also passed.
This parser-only regression run does not establish new GPU decode support.
