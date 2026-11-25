# AVS / AVS+ video decoder, DRA audio decoder and media source for Media Foundation

One DLL, three cooperating Media Foundation components, all decoding/demuxing with
FFmpeg 9.1 statically linked inside:

```
file  ->  CAVSByteStreamHandler  ->  CAVSMediaSource  --- compressed samples --->  CAVSVideoDecoderMFT  ->  NV12 / I420 / YV12 / P010
        (HKLM extension mapping)    (libavformat: .ts,  (MFVideoFormat_AVS /        (libavcodec + libcavs)
                                    .avs, ...)          MFAudioFormat_DRA)          CAVSAudioDecoderMFT  ->  PCM / float
```

Windows cannot open these files by itself: MF's TS handler does not know stream type
0x42 (AVS video) nor the `'DRA1'` registration descriptor, and a raw `.avs` elementary
stream has no handler at all.  So the decoders *and* the source are needed before any
player can play the file.

Files
-----

| file                                | what it is                                                             |
|-------------------------------------|------------------------------------------------------------------------|
| `AVSVideoDecoder.vcxproj` / `.sln`  | VS project (x64 only), also built by `scripts\build-mft.cmd`           |
| `classGuids.h`, `Guids.cpp`         | CLSIDs plus the AVS / AVS2 / AVS3 / DRA sub-type GUIDs                 |
| `AVSVideoDecoderMFT.{h,cpp}`, `_Process.cpp` | video decoder MFT: negotiation, stream change, process loop    |
| `FFmpegDecoder.{h,cpp}`             | libavcodec wrapper: open, decode, colour convert, presentation reorder  |
| `AVSAudioDecoderMFT.{h,cpp}`        | DRA audio decoder MFT (PCM 16 and float 32 output)                      |
| `FFmpegAudioDecoder.{h,cpp}`        | libavcodec wrapper for DRA -> interleaved PCM, audio timeline            |
| `AVSMediaSource.{h,cpp}`            | `IMFMediaSourceEx` + `IMFByteStreamHandler` over libavformat            |
| `DllMain.cpp`, `AVSVideoDecoder.def`| COM server, `MFTRegister`, byte stream handler registry entries          |

Build
-----

```
scripts\build-ffmpeg.cmd          FFmpeg 9.1 + libcavs + libdra -> ff91-install
scripts\build-mft.cmd             AVSVideoDecoder.dll + the console tools
scripts\build-mft.cmd debug       debug version
scripts\build-mft.cmd tools       only the console tools
scripts\run-tests.cmd             the checks listed under Tests below
```

`scripts\port_avs_to_ffmpeg.py` copies `libcavs.c/h`, `libcavsdec.c`, `dradec.c/h`,
`libdradec.c` and the MSVC pthread shim `compat/avs/pthread.h` into a stock FFmpeg 9.1
tree and patches `configure`, `libavcodec/Makefile`, `allcodecs.c`, `codec_id.h`,
`codec_desc.c`, `defs.h`, `profiles.[ch]`, `libavformat/mpegts.c`.  The DRA decoder is
then enabled as `--enable-decoder=libdra`.  Three FFmpeg details cost time when
porting by hand and are handled by the scripts:

* new codec ids must be **appended** to the enum in `codec_id.h`: `libavcodec/version.c`
  static-asserts the value of ids spread over the list ("Don't insert new codec ids in
  the middle of a list");
* `codec_descriptors[]` in `codec_desc.c` is searched with `bsearch()`, so a new
  descriptor must be inserted where its id sorts.  Put in the wrong place it is simply
  not found: streams are not recognised, or the library misbehaves;
* turning components on or off does not rebuild the object tree reliably (the generated
  `*_list.c` files are `#include`d and `config.h` is not a prerequisite of every `.o`),
  which shows up much later as unresolved externals in the MFT link.  `build_ffmpeg.sh`
  therefore drops all objects when the configure options or any of the ported files
  change (`--clean` forces it).

Tests
-----

Each component can be exercised on its own, which is how a source bug is told apart
from a decoder bug.

```
bin\avsmft-test.exe   samples\avs_videoonly.ts out\mft_nv12.yuv -mft <dll> -frames 60
                      video MFT alone; -direct decodes with libavcodec for reference,
                      -fmt nv12|i420|yv12|p010 requests a specific output format,
                      -types lists GetOutputAvailableType, -nosize forces a stream change

bin\avsaudio-test.exe CCTV-4.ts out\dra.wav -mft <dll>          DRA MFT alone
                      -direct for the libavcodec reference, -trace per packet/frame

bin\avssource-test.exe <file> [-raw] [-video|-audio] [-frames N] [-out f]
                             [-nv12|-i420|-yv12|-p010] [-float] [-bmp N]
                      default: the source + SourceReader + the registered MFTs
                      -raw:    compressed samples only, i.e. the source alone
                      -handler: CAVSByteStreamHandler driven by hand (no registry)
                      -url:    MFCreateSourceReaderFromURL, the path a player takes
                      the requested decoded type carries the stream's colour signalling
                      and channel mask over, like a player does, so the dumps show
                      whether the pipeline propagates them

bin\mftenum.exe <input-subtype> [video|audio]   what MFTEnumEx sees
bin\mfprobe.exe  <file>                         what plain MF can open
```

`scripts\run-tests.cmd` runs eleven checks.  All of them pass on this machine; the ones
that compare output do it byte for byte:

| #  | what                                                       | result                        |
|----|------------------------------------------------------------|-------------------------------|
| 1  | video MFT alone, AVS in MPEG-TS                            | 120 frames, presentation order |
| 2  | source + reader + MFT on the same file                     | identical to 1                |
| 3  | raw `.avs` elementary stream, no container                 | 40 frames, starts at 0        |
| 4  | DRA through libavcodec (reference)                         | 1396 frames, 5718016 B PCM    |
| 5  | source + reader + DRA MFT                                  | identical to 4, gaps: none    |
| 6  | DRA MFT alone                                              | identical to 4                |
| 7  | H.264 in TS through the custom source, system decoder      | works (control case)          |
| 8  | `MFTEnumEx` for AVS and DRA inputs                         | both registered               |
| 9  | byte stream handler object, no registry                    | identical to 3                |
| 10 | `MFCreateSourceReaderFromURL` through the registry         | identical to 3                |
| 11 | MFPlay: real topology + session + EVR (11a raw `.avs`,     | plays, clean shutdown         |
|    | 11b AVS video + DRA audio in TS)                          |                               |

Register
--------

```
regsvr32 "MediaFoundationTransform\AVSVideoDecoder\bin\x64\Release\AVSVideoDecoder.dll"
```

`DllRegisterServer` writes (HKLM, so it must run elevated):

* the four COM classes (two MFTs, the media source, the byte stream handler);
* the two MFTs in their decoder categories, video: in AVS/AVS2/AVS3 -> out
  NV12/I420/YV12/P010 (the 10 bit format last, so a sink that does not choose
  explicitly still gets 8 bit), audio: in DRA -> out PCM/float;
* one **sub-key per extension** under
  `HKLM\SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers`, containing a
  value whose *name* is the handler CLSID - that is exactly how Windows registers
  `.mp4`, `.mkv`, `.ts`.  A value on the parent key or the (Default) value of the
  sub-key is silently ignored, which shows up as `MF_E_UNSUPPORTED_BYTESTREAM_TYPE`
  (0xC00D36C4) from `MFCreateSourceReaderFromURL`;
* `.avs .cavs .avs2 .avs3` by default.  `.ts` / `.m2ts` are only added when the
  machine-level variable `AVS_MFT_REGISTER_TS=1` is set, because taking over `.ts`
  makes every TS file go through this source.

`AVS_MFT_TRACE=1` in the environment of the player makes the source log to stderr what
MF called and what it returned - the only practical way to see a registration problem.

Object lifetime and DllCanUnloadNow
-----------------------------------

Every COM class in this DLL (video MFT, audio MFT, media source, input stream, byte stream
handler) derives from `BaseObject` from `Common/MFClassFactory.h`, which takes the
class-factory server lock in its constructor and releases it in its destructor.  That is what
makes `DllCanUnloadNow()` answer `S_FALSE` while one of our objects is still alive.

This is not decoration.  A real Media Session was traced calling `DllCanUnloadNow()` after it
had released the video decoder but while the media source was still alive:

```
[avsmft] ~video decoder this=...
[avsmft] DllCanUnloadNow -> S_FALSE (objects of this DLL are alive)
```

Without the server lock the same point in the same graph answered `S_OK`, which tells the host
it may unmap a DLL that still owns live objects - the next vtable call then lands in code that
is no longer loaded.  Windows does **not** consult `DllCanUnloadNow()` on `FreeLibrary()`, so
the host has to ask first; that half of the contract belongs to the host.

When a new COM class is added, it must derive from `BaseObject` (or otherwise take the server
lock), otherwise this contract silently breaks for it.  `bin\mftlifecycle-test.exe` checks both
directions of the contract plus load/create/close cycles for all four exported classes:

```
bin\mftlifecycle-test.exe <path>\AVSVideoDecoder.dll [-clsid video|audio|source|handler] ^
    [-mode all|factory|instance|unloadalive|leak|rude] [-repeat 20] [-file some.avs]
```

`leak` exits the process with a live instance (closing a player without releasing); `rude`
documents the host-side half.  The suite runs this as item 11g.

Decoder threading
-----------------

MF calls an MFT from one thread at a time, but that does not oblige the *decoder*
to be single threaded: libavcodec always hands pictures back in presentation order,
whatever it does internally.  The video wrapper therefore leaves
`AVCodecContext::thread_count` at 0 ("the decoder decides"), which is what makes
`libdavs2` and `libuavs3d` spread a stream over all cores.  The ported libcavs (AVS+) runs its own thread pool and is not
affected by this.  `AVS_MFT_THREADS=<n>` overrides the policy (`1` restores the old
single threaded behaviour) and is only meant for measurements:

```
scripts/bench-mft.sh "<file>" [frames]        # one thread vs the default policy
```

### Measured decode throughput

Every number below was produced by one command that is still runnable today, prints its own
picture accounting, and is quoted in the table.  A throughput figure without that command is
not kept here: round 40's table had none, and round 41 then found the same path discarding
69 % of the pictures, which is exactly the kind of number a command would have exposed.

```
scripts/throughput-measure.py <file> <frames> <runs> default= threads1=AVS_MFT_THREADS=1
```

Method (it bounds what may be claimed): our own media source plus the decoder MFT through a
real `IMFSourceReader`, **no renderer**; the wall time includes process start and decoder
initialisation, so short runs read pessimistically.  The script prints delivered pictures and
the MFT's own `total dropped` counter per run, and takes the median of the runs.  Measured
here on 16 logical CPUs, 2026-11-25; every row had `dropped 0` and the two thread policies
were compared plane by plane with `scripts/bench-mft.sh`, bit identical:

| stream (declared source rate) | `AVS_MFT_THREADS=1` | default (decoder decides) | runs |
|---|---|---|---|
| AVS3 1920x1080, 200 frames (29.97 fps) | 114.02 fps = 3.80x | **178.25 fps = 5.95x** | 3 interleaved, +-1 % |
| AVS2 1920x1080, 200 frames (29.97 fps) | 68.03 fps = 2.27x  | **130.63 fps = 4.36x** | 3 interleaved, +-1 % |
| AVS2 3840x2160 10-bit, 100 frames (50 fps) | 29.66 fps = 0.59x | **39.42 fps = 0.79x** | 3 interleaved, +-3 % |
| AVS3 3840x2160 HDR, 100 frames (30 fps) | 33.01 fps = 1.10x | **38.55 fps = 1.28x** | 3 interleaved, +-2 % |
| AVS3 7680x4320 10-bit HDR, 30 frames (25 fps) | **4.86 fps = 0.19x** | 3.28 fps = 0.13x | 3 interleaved |

What these numbers say, and what they replace:

+ the older sentence here claimed that a single threaded AVS2 or AVS3 decoder "cannot keep
  up with 1080p, let alone the 4K/8K samples".  Measured, it is wrong at every rate up to
  4K: single threaded libuavs3d is 3.8x and libdavs2 2.3x the source rate.  8K is the only
  row that misses real time, by 5-7x - and it misses it because of throughput, not because
  anything refuses the file.
+ **the default thread policy wins at 1080p and 4K (1.3-2.0x) and loses at 8K (1.5-1.9x
  slower than one thread)**.  The direction is stable across two independent batches of
  interleaved runs; the magnitude is not (8K default alone measured 2.50 and 3.28 fps in
  the two batches).  So the 8K thread policy is an open question, not a settled improvement.
+ holding the pictures of an oversized decoder burst costs memory: the 8K run above peaked at
  **3151 MB** working set (`bin/avssource-test.exe … -frames 30`, sampled with
  `tmp/peakmem.ps1`).  That is a deliberate trade for not losing pictures, and it needs a
  byte budget - see "Known defects" below.
+ the isolated-MFT benchmark (`scripts/bench-mft.sh <file> <frames> <w> <h>`) agrees with
  these figures on AVS3 1080p (171 fps parallel vs 178 fps through the media source) and
  its two thread policies produce bit-identical output.  It is **not** consistent for the
  AVS+ sample and that difference is unattributed; no AVS+ throughput figure is claimed here
  until it is.
+ **short runs understate the steady state, because the fixed cost is real.**  The same file
  measured at two frame counts (`scripts/throughput-measure.py <file> 5 3 default=` and the
  same call with 450) costs **0.42 s before any picture is useful** and then 3.06 ms per
  frame (450 frames: 252 fps steady), so the 200-frame row above (178 fps) is a mixture of
  the two.  The ceiling for that stream in an independent harness (reference
  `ffmpeg -nostdin -v error -i <file> -frames:v 450 -f null -`, libuavs3d choosing its own
  threads) is **2.64 ms per frame**: at 1080p we are within 16% of it.  At 8K the same
  comparison says the gap is ours, not the decoder's: reference fixed 1.21 s + 42.6 ms per
  frame, ours fixed 3.4-5.6 s + 112 ms per frame (`AVS_MFT_THREADS=1`) or 244 ms per frame
  (default policy).  So the remaining work is on our side of the line, and it is not
  bit-stream GPU decode: the Vulkan reconstruction path measured 44.8x slower end to end and
  its kernels alone were 4.9x slower than the whole CPU path (rounds 21-25).  Where a GPU is
  useful here is presentation work (colour conversion, PQ mapping, de-interlacing, zero-copy
  surfaces), which this MFT currently blocks by answering `MFT_MESSAGE_SET_D3D_MANAGER` with
  `MF_E_INVALIDREQUEST`.

Known defects
-------------

Each entry names the check that would notice it going away; "fixed" in a comment is not
enough, because a defect that nothing observes comes back.

+ **Holding an oversized decoder burst costs memory (open, round 41).** The synchronous path
  now keeps the pictures instead of dropping them (regression 11t asserts `container access
  units == delivered + dropped`, with `dropped == 0`).  The measured cost: 30 frames of the
  7680x4320 10-bit HDR sample peak at **3151 MB** working set.  A byte budget across queue +
  held frames is still to be added; if a frame-count budget makes 11t red at 1080p, the
  budget has to be per-resolution rather than per-count.
+ **The default thread policy is slower at 8K (open, reproduced twice, unattributed).**
  7680x4320: default 2.50 / 3.28 fps vs `AVS_MFT_THREADS=1` 4.84 / 4.86 fps across two
  interleaved batches - the direction is stable, the ratio (1.5-1.9x) is not, and at
  1080p/4K the default policy is the faster one by 1.26-2.0x.  Reproduce:
  `scripts/throughput-measure.py video/playable/CCTV8K.AVS3.MP2.CB_10bit_HDR.avs3 30 3 default= threads1=AVS_MFT_THREADS=1`.
+ **End-of-stream tail on one sample (open, pinned by shape).** With `AVS_AEC_ZERO=1` on
  `samples/CCTV-4.avs`, all 700 pictures arrive and the first 698 match the reference, but
  indices `[698, 699]` differ in content (not an order swap: the reference's last hash is
  absent from our output).  Check 11f records that exact shape as a KNOWN defect and goes red
  for any other shape.  Trace pointer: in that configuration the reorder state never closes
  (`held` reaches 49, then `flush gave 50 picture(s)`) and the last picture's timestamp has no
  input sample to pair with (`matched=0`).
+ **`-frames N` does not stop input feeding in the isolated harness (open, tool).**
  `-frames 20` and `-frames 60` both report `fed 750 access units` on `samples/CCTV-4.avs`,
  so isolated-benchmark per-frame times include decoding the whole file.  That is why
  `scripts/throughput-measure.py` (media source path, prints `total dropped`) is the
  throughput entry point and `scripts/bench-mft.sh` is only for equality and relative ratios.
+ **Logged frame rate is printed wrong (open, log only).** `offered: ... fps=30000.1001`
  means `30000/1001` = 29.97; the sample timestamps really step 33.0 ms, so the path is
  correct and the log line lies.

Closed and still watched: pictures thrown away in the synchronous path (450 access units,
139 delivered, 311 self-reported drops) - fixed in round 41, pinned by 11t; the isolated
harness treating `ProcessInput: 0xC00D36B5` (`MF_E_NOTACCEPTING`, i.e. flow control) as fatal;
the ordering defect at the end of broadcast `.ts` (round 19, 11h).

There is no hardware **bit-stream** decode for AVS: neither NVDEC, nor Vulkan Video,
nor QuickSync defines an AVS/AVS2/AVS3 profile, and `davs2`, `uavs3d` and libcavs are
CPU decoders (the FFmpeg tree has no hwaccel for `cavs`/`avs2`/`avs3`).  What this tree
adds is a different thing: an optional Vulkan **compute** path that reconstructs inside the
decoder - motion compensation, the transform and the loop filter.  It is off by default and
opt-in with `AVS_VK=1`; its output is bit-exact against a direct libavcodec decode (check
11k), and what happens when that backend is unavailable or stops working is measured rather
than assumed (check 11p, see the integration note below).  It is not a performance feature:
the performance direction was closed under pre-registered conditions (re-entry would need a
motion-compensation acceptance of 90% or more with the CPU writing 4 GB or less).
Separately, the MFT still answers `MFT_MESSAGE_SET_D3D_MANAGER` with `MF_E_INVALIDREQUEST`,
so sharing decoded surfaces with a GPU colour conversion or a P010 path
in `QueueOutputFrame()` is still to be built.

Custom media types
------------------

Media Foundation has no sub-type for AVS or DRA, so the DLL defines fourCC style GUIDs
(see `classGuids.h`):

```
MFVideoFormat_AVS   'AVS '  ->  AV_CODEC_ID_CAVS   (AVS1-P2/P16, AVS+)
MFVideoFormat_AVS2  'AVS2'  ->  AV_CODEC_ID_AVS2   (needs libdavs2/libxavs2)
MFVideoFormat_AVS3  'AVS3'  ->  AV_CODEC_ID_AVS3   (needs libuavs3d)
MFAudioFormat_DRA   'DRA '  ->  AV_CODEC_ID_DRA    (GB/T 20090.6)
```

The source also publishes H.264 / HEVC / MPEG-2 video and AAC / MP3 audio with the
standard MF sub-types when libavformat recognises them, so a normal file routed
through this source still reaches the system decoders.

Timelines
---------

* time zero is the earliest **presentation** time, not the container's `start_time`:
  with B pictures the first packet in the file decodes before it displays, so using the
  first packet makes the first picture negative.  The source scans the first packets
  (`PrerollScan`), keeps them, and rebases on the smallest PTS.
* the pipeline's sample times are the authority for the decoder.  FFmpeg's elementary
  stream parser invents its own monotonic times for a raw AVS stream, which is why the
  reorder in `FFmpegDecoder` uses the times the samples carry and only sorts when every
  held picture really has one (a missing time stamp is `-1`, never 0).
* DRA audio is stamped on a continuous internal timeline: the DRA packets of
  `CCTV-4.ts` do not decode to a fixed number of frames (one packet yields two or
  three 1024-sample frames, the extra ones with no time stamp), so the decoder
  advances by the frame duration and re-anchors only when a packet time is off by more
  than five frames - which means a seek or damage, not normal packing variation.
* Raw elementary streams have no container timeline either: FFmpeg's raw demuxers are
  `AVFMT_NOTIMESTAMPS`, and the packets they do hand out all carry the *same*
  synthesized pts.  `PrerollScan` therefore checks whether a stream's packet time stamps
  actually differ; when they do not, `CAVSInputStream` builds the timeline from the frame
  rate the probing found (25 fps when nothing is known) and gives the samples matching
  durations.  `FFmpegVideoDecoder` also prefers those MF sample times over the picture
  time stamps libavcodec invents for elementary streams when a picture cannot be matched
  to a buffered sample.  Before this, a raw `.avs3` file was stamped entirely at t = 0:
  the presentation clock never moved.

Asynchronous MFT mode (`AVS_MFT_ASYNC=1`)
-----------------------------------------

The decoder can run in two shapes, and both decode the same bit stream:

* **synchronous (default)**: a plain `IMFTransform`.  The pipeline calls `ProcessInput`,
  polls `GetInputStatus` / `GetOutputStatus` and calls `ProcessOutput`.  This is the
  shape of the MSDN sample MFTs and it is what the `IMFSourceReader` path and every
  regression check use.
* **asynchronous** (`AVS_MFT_ASYNC=1`): `GetAttributes` reports `MF_TRANSFORM_ASYNC`
  (and `MF_TRANSFORM_ASYNC_UNLOCK = FALSE`, because the decoder wrapper is not free
  threaded), the MFT also implements `IMFMediaEventGenerator`, and the decoding runs on
  an internal worker thread.  `ProcessInput` only copies the access unit into a small
  queue, so the events are raised from another thread, which is what the session's
  asynchronous MFT proxy expects: `METransformNeedInput` before the pipeline may feed
  the next unit (exactly one per consumed input), `METransformHaveOutput` per decoded
  picture, `METransformDrainComplete` when a drain finished.

Measured behaviour: the session really does drive the MFT through that protocol - if the
first `METransformNeedInput` is not sent, the pipeline never calls `ProcessInput` at
all, and with it the whole stream flows (for `samples\CCTV-4-cut3.avs`: 76 access units
into the MFT, 67 pictures handed on to the sink, while the earlier "post an event per
ProcessInput" variant stalled after 22 units).  It does **not** change the end-of-stream
behaviour: the topology still reports `ended(400)` and `MESessionEnded` is still
missing, which is why asynchronous mode stays off by default.  `bin\avsmft-test.exe` drives the synchronous
protocol only; pointed at an asynchronous instance it stops with `MF_E_NOTACCEPTING`
(`0xC00D36B5`) when its own feed outruns the queue, which is expected rather than a
bug in the decoder.

Notes / open points
-------------------

* The **media session** path is exercised by check 11: 11a is a raw elementary stream,
  11b (`samples\CCTV-4-short2-ts.avs`, MPEG-TS with AVS video + DRA audio) is the real
  two stream case.  Both resolve a topology, play, consume every access unit of both
  streams and shut down cleanly.  `bin\mfsession-test.exe` goes one level deeper: it
  builds its own topology (source node + EVR) and prints the session events and the
  `IMFClock` state.  With it AVS, AVS2 and AVS3 fixtures now reach
  `topology=ready(100)`, start, and the clock advances with the wall clock while every
  decoded frame is handed to the graph (`[avsmft] handed to graph #N`).  What is still
  missing is the *end*: `MESessionEnded` is not produced for an AVS stream.
  **This is not a property of "a custom decoder"**: `scripts\attribution-matrix.sh` runs
  the 2x2 matrix ({AVS, H.264} x {our MFT as an explicit node, whatever the loader picks})
  and our own MFT decoding H.264 (`AVS_MFT_ALLOW_H264=1`, needs a FFmpeg built with
  `AVS_MFT_TEST_H264=1`) posts `MESessionEnded` normally.  **Round 27: that control is not
  reproducible with the current build** - measured `samples delivered = 0`,
  `end-of-presentation=-1`, `topology-ended=-1` for H.264 through our own source and MFT, so
  it delivers nothing and proves nothing either way.  The control that IS reproducible uses no
  component of ours: `samples\control_h264.mp4` through the system source, the system H.264
  decoder and the same sink posts `MESessionEnded` in the same harness
  (`bin\mfsession-test.exe samples\control_h264.mp4 -seconds 40 -grabber` ->
  `session-ended=yes`).  That proves Media Foundation does post the event here, but it cannot
  separate "subtype" from "our decoder implementation" - the only dimension left - because the
  control that would do that is the broken one above.  Ruled out by that measurement:
  "our MFT contract", two-stream/DRA audio (a video-only AVS raw stream behaves the same),
  and the environment.  The earlier lead on the *media timeline* ("AVS carries far more
  access units than pictures") is **dead**, measured on one timeline by
  `scripts\eos-table.py`: the H.264 control also has more units than pictures (63 units,
  60 pictures), and in all three cells the clock stops within ~30 ms of the last picture's
  presentation time (`-0.03 s` / `+0.01 s` / `-0.03 s`).  What the passing and the failing
  cell share, moment by moment (`bin\mfsession-test.exe -ts` event timeline):
  `MEEndOfPresentation` at ~3.1-3.3 s, `topology=ended(400)` about 200 ms later - and then
  the H.264 cell posts `MESessionEnded` **in the same millisecond** as `topology=ended`,
  while the AVS cell posts nothing further; there the session stops the presentation clock by
  itself (observed across 34 clock polls, all before the harness calls `Stop()`).  Caveat so
  nobody over-reads that: the passing cell's clock state *after* its end is not observed, the
  harness leaves its poll loop on `MESessionEnded`, so "clock stopped" is not the contrast -
  the only observable difference is the missing `MESessionEnded`.
  Neither cell raises `MF_E_TRANSFORM_STREAM_CHANGE`: both decoders now say so explicitly
  (`raising MF_E_TRANSFORM_STREAM_CHANGE`) and that trace has a positive control
  (`bin\avsmft-test.exe ... -nosize` raises exactly 1, matching the tool's own count).
  **Round 38: the entry point the built-in Windows player actually uses was measured, and it
  explains the "unsupported format: unknown" message.** Windows 10's Movies & TV drives the
  **Media Foundation Media Engine** (`IMFMediaEngine`) - not `MFCreateSourceReaderFromURL`,
  not MFPlay - and that component has its own format-acceptance step. Asking it directly
  (`bin\mediaengine-probe.exe`, pinned by `scripts\player-path-test.py` = regression 11r, one
  process per file, H.264 control required): a bare `.avs` is taken and decoded by us
  (720x576); a `.ts` whose only video is AVS3 is taken and decoded by us (480x270 10-bit,
  and 3840x2160); **AVS3 inside `.mkv` gives `native video=0x0` - audio only - and our
  handler is called 0 times**, because the system MKV handler claims Matroska by content.
  Renaming the `.mkv` to `.avs3` does not change that (tested on 4 files). What works, and was
  verified through the Media Engine itself rather than through a harness:

      ffmpeg -i in.mkv -map 0:v:0 -c:v copy -f avs3 out.avs3

A convenience tool does the repack for a whole folder: it decides the codec from what OUR
  media source offers (not from the extension), copies the video track without re-encoding, and
  verifies each result through the Media Engine:

      python scripts/repack-playable.py "video/avs2_avs3_test_video" "video/playable" --verify

  Measured on that folder: 22 inputs produced 19 distinct output files (3 inputs are the same
  programme in two containers) and **all 19 verified playable through the player's own
  component** - 4K AVS2/AVS3 HDR, 1080p, 10-bit, raw AVS+; 4 skipped (no AVS video inside);
  0 failed. The originals are never modified. Two facts that cost time: the raw AVS+ muxer of
  that build is named `cavsvideo`, not `cavs`, and it initialises from the stream's dimensions,
  so a broadcast stream without a leading sequence header fails with "[cavs] Dimensions
  invalid"; and that build crashes on some AVS files NON-deterministically (same file, same
  command: 4 successes and 1 segfault over five attempts), so the copy chain in
  `scripts/avs_copy.py` tries `-f <muxer>` then `-f data` (measured byte-identical to
  `-f avs2` on one file) then an MPEG-TS wrapper, with retries, and is shared by this tool and
  by the 11r guard so the two cannot drift apart.

  Two rules this round produced: probe **one file per process** (several files in one process
  changed the verdict of the later ones - an `.mkv` that reported CANPLAY alone reported
  error 4 after its neighbours), and treat "the engine refused it" as meaningless unless the
  H.264 control was taken in the same run, since no renderer is attached.

  **Round 36: "decode it whatever wrapped it" is now a measured contract, and the half
  that the MFT cannot fix is measured too.**

  What the decoder does does not depend on the container, and `scripts/container-decode-test.py`
  (regression 11q) now checks that file by file. "Contains AVS" is decided by the INDEPENDENT
  reference FFmpeg build, never by the file name; for every such file our media source has to
  offer the matching subtype and decode at least one frame. Measured over
  `video/avs2_avs3_test_video` and `samples/`: 37 files carry an AVS bitstream, 36 decode,
  0 fail, and 1 is a broken fixture (the reference itself says `[cavs] Dimensions invalid`).
  That covers MKV, MPEG-TS and bare elementary streams, AVS2 and AVS3, 720x404 up to
  7680x4320, 10 bit and HDR. No repacking, no renaming.

  What the MFT cannot fix, and this is now measured rather than argued: `bin/typedump.exe`
  (new, `tools/typedump.cpp`, built by `scripts/build-mft.cmd`) goes through
  `MFCreateSourceReaderFromURL` - the path a system player takes - and prints the native media
  types. For every AVS .mkv the system MKV handler reports **only the audio streams**: it
  drops the AVS video stream entirely, without even exposing an unknown video stream. There is
  no input type for a decoder MFT to match, so declaring more input types cannot help. The
  demuxing has to be done by a source that understands AVS, which is ours.

  How MF chooses, from the same tool: we are the **fallback**, not the first choice.

  | file | what the system handler does | our handler called | MFPlay |
  |---|---|---|---|
  | `MarketPlace_480x270_60_10bit_ra_avs3.ts` (no playable stream it recognises) | fails | 474 calls | `480x270`, plays, 71 pictures |
  | `北京冬奧片花...avs3.ts` (has AAC) | succeeds, keeps audio, drops video | 0 calls | `0xC00D36B2`, no picture |

  So: if the system's own handler can identify *any* playable stream in the file, it keeps the
  file and we are never asked. Registering our handler ahead of it is not available -
  the keys for `.ts/.m2ts/.mkv/.mp4` refuse an Administrator write (`reg add` says
  "access denied", `RegSetValueEx` returns `0x80070005`), and writing the same values under
  `HKLM\SOFTWARE\Classes\MediaFoundation\ByteStreamHandlers` or `HKCU\...` succeeds but
  Media Foundation does not read those (0 handler calls). Only the extensions nobody else
  owns - `.avs/.cavs/.avs2/.avs3` - can be registered.

  Two fixes that came out of reading this properly:

  * `DllRegisterServer` used to stop its extension loop at the **first** refused key, so the
    rest of the list was never attempted and the whole call returned a failure even though both
    decoders had registered. That is how "we registered .ts" survived here for many rounds
    without ever having been true. Each extension is now attempted on its own and reported;
    `AVS_MFT_REGISTER_CONTAINERS=1` (alias of the old `AVS_MFT_REGISTER_TS=1`) additionally
    asks for `.ts/.m2ts/.mkv`.
  * `avsmft-register` now reads the registry back and prints what Media Foundation will
    actually consult, plus a self-check that the handler CLSID points at the DLL it was given:

      ```
      [avsmft] handler registration for .avs: accepted (0x00000000)
      [avsmft] handler registration for .ts: refused (0x80070005)
      DllRegisterServer -> 0x00000000 OK
      byte stream handler read-back (this is what MF will consult):
        .avs  : ours=yes  other handlers=0
        .mkv  : ours=no   other handlers=1   <- owned by the system; an Administrator write is refused
      ```

  **Round 33: what "the system player cannot open it" actually means - four separate layers.**
  Each one was measured; only one of them was a product gap.

  | layer | check | result |
  |---|---|---|
  | MFT registered | `HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\9e5d6a1b-...3b11` | present, with Input/Output types |
  | in the decoder category | subkey of `Transforms\Categories\d6c02d4b-...` | present |
  | `MFTEnumEx` finds it | `bin\mftenum.exe` | listed among the 10 video decoders |
  | byte stream handler | `ByteStreamHandlers\.avs` | present |
  | **MF resolves a media source** | `bin\mfprobe.exe file.avs` (`flags=0x11`) | `CreateObjectFromURL -> 0x00000000, object type=0`, native type `'AVS '` |
  | **playback through MFPlay** | `bin\mfplay-test.exe file.avs -seconds 10 -no-end` | `native video size: 720x576`, **`playback through the MF graph OK`**, 26 pictures handed to the graph |
  | **Windows file association** | `assoc .avs` | **was empty** - double-clicking started nothing |

  So the playback pipeline works; what was missing is the association Windows needs before it will
  start a player at all. `HKLM\SOFTWARE\Classes\.avs` does not get created by `DllRegisterServer`
  (that is a shell association, not a Media Foundation one) - an installer has to write
  `HKCU\Software\Classes\.avs` = a ProgID, plus the ProgID's `shell\open\command`.

  **Two things to know before you conclude "it does not play":**

  1. **The extension must match the content.** `samples\CCTV-4-short.ts` (AVS in MPEG-TS, `.ts`
     name) plays; `samples\CCTV-4-short2-ts.avs` (the same kind of content under an `.avs`
     name) fails with `0xC00D36B2` and `native video size 0x0`.  Media Foundation picks the byte
     stream handler from the extension, and our handler probes the content: `.avs` + TS and
     `.ts` + raw elementary stream are both ambiguous.  Name a transport stream `.ts`/`.m2ts`.
  2. **`mfplay-test` without `-no-end` prints `playback did NOT work` even when it played.**
     Its pass criterion is "the graph ended"; this MFPlay build does not expose `GetPosition`
     and never reports `MESessionEnded` to a host process, so `media session events seen: 0`.
     Read the evidence instead: `native video size`, `handed to graph` count, and the
     decoder's own `emitted #N` trace.  With `-no-end` the tool reports
     `playback through the MF graph OK`.

  On this machine there is no classic player to launch (`wmplayer.exe` is absent) and the
  system player is the UWP `Microsoft.ZuneVideo`, whose `Video.UI.exe` refuses `Start-Process`
  ("access denied" - UWP has to be activated through the shell), so the double-click path
  itself could not be exercised here; everything below that layer was.

  **Round 32: the end-of-playback contract is now verified with a real renderer too.**  The
  decoder was registered into the system (`bin\avsmft-register.exe <dll>` elevated, then
  `bin\mftenum.exe` lists `{314FFBAE-5B41-4C95-9C19-4E7D586FACE3} AVS/AVS+/AVS2/AVS3 Video
  Decoder`) and the EVR activates again (`bin\probe-evr.exe`: `ActivateObject -> 0x00000000`).
  In `scripts\eos-signals-test.py --sink renderer`, twice in a row:

  | cell | clock-max | media end | `MEEndOfPresentation` | `topology=ended` | `MESessionEnded` |
  |---|---|---|---|---|---|
  | AVS raw ES | 3.071 / 3.069 s | 3.04 s | 3.329 s | 3.514 s | no |
  | AVS in MPEG-TS | 4.360 / 4.361 s | 4.33 s | 4.351 s | 4.766 s | no |
  | system H.264 | 29.947 / 29.941 s | 29.92 s | 29.835 s | 29.999 s | **yes** |

  So the AVS pictures really do play to the end of the content on a real renderer, and the
  same-source difference still holds there: with everything else identical, the system-known
  subtype posts `MESessionEnded` and the AVS subtypes do not.  `bin\mfplay-test.exe` also
  opens the file now (`native video size: 720x576`, `failure: 0x00000000`, previously
  `0xC00D36B2`) and the decoder's own trace shows all 26 pictures emitted and handed to the
  graph; MFPlay itself reports `ended: no` only because this harness cannot subscribe to its
  internal session event queue, not because playback stopped.

  **Round 28 closes the attribution.**  The one control that was missing - our own MFT fed a
  stream that is *not* AVS - now runs, because `scripts/build_ffmpeg.sh` already had the switch
  (`AVS_MFT_TEST_H264=1`) and nobody had ever built with it.  With it the control delivers
  60 pictures and **posts `MESessionEnded`**, in the same source, the same MFT class, the same
  harness and the same sink, with the AVS cells still not posting it:

  | stream through our source + our MFT | delivered | `MEEndOfPresentation` | `topology=ended` | `MESessionEnded` | subtype |
  |---|---|---|---|---|---|
  | H.264 in MPEG-TS | 60 | 3186 ms | 3437 ms | **yes** | `{3F40F4F0-...}` `MFVideoFormat_H264` |
  | AVS raw ES | 26 | 2936 ms | 3058 ms | **no** | `{20535641-0000-0010-...}` `'AVS1'` |
  | AVS in MPEG-TS | 19 | 3980 ms | 4332 ms | **no** | `'AVS1'` |
  | AVS, 700 pictures | 700 | 29890 ms | 30014 ms | **no** | `'AVS1'` |

  The subtype GUIDs above are read from the run itself (`subtype: {...}` in the stream
  descriptor dump), so this is a directly observed difference, not an inference.  Everything
  else is held constant: the same clock reaches the end of the timeline in every cell (within
  13 ms of the last picture), the picture count is complete, and `topology=ended(400)` arrives
  in all four.

  **The contract item, in a form that can be checked without knowing Media Foundation's
  internals**: a topology whose video subtype is one the session does not recognise for
  end-of-presentation purposes will report `MEEndOfPresentation` and reach
  `topology=ended(400)` and stop its clock at the end of the content - and then **post nothing
  further**; it will not post `MESessionEnded`.  The check is: (1) `MEEndOfPresentation`
  present, (2) `topology=ended(400)` present, (3) `clock-max` within 20 ms of the last
  sample's presentation time, (4) picture count complete - if all four hold and
  `MESessionEnded` is still absent, this item applies.  All four AVS fixtures here satisfy it;
  the H.264 control satisfies all four and *does* post the event.

  **What this does not mean**: AVS playback is not broken.  Every AVS cell here reaches the
  end of its content and delivers every picture.  What is missing is the single event a host
  uses to decide "the presentation is over - close, or advance the playlist".  A host that
  needs that decision has to treat `MEEndOfPresentation` + a stopped clock as its end signal,
  or use a system sub-type for the stream; there is no decoder-side fix, because the control
  above shows the decoder is not what decides.

    The defect boundary is therefore: after the topology reports "ended", the session does not
  call the presentation finished - with the same source, the same MFT class and the same EVR
  sink, the only remaining difference being the bit stream and its subtype.  check 11i pins
  that state.  Ruled out by measurement: `MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE`
  (see `AVS_MFT_DYNAMIC_FORMAT`), input `hnsMaxLatency`, and
  `MFT_OUTPUT_DATA_BUFFER_INCOMPLETE`, `ProcessEvent` returning `MF_E_INVALIDREQUEST`
  (the graph does not send process events at all), `MF_MT_FIXED_SIZE_SAMPLES` /
  `MF_MT_SAMPLE_SIZE` on the output type, marking the last output sample with
  `MFT_OUTPUT_DATA_BUFFER_STREAM_END` (`AVS_MFT_DRAIN_EOS=1`), holding the newest
  picture back until the pipeline drains so that the last sample really can carry that
  marker (`AVS_MFT_HOLD_LAST=1`, the trace then shows `marked the last output sample as
  stream end`, frame count and pixels unchanged), inserting the decoder
  as an explicit topology node (`bin\mfsession-test.exe -dec ...`), giving the instance
  an `MFTransformCategory = MFT_CATEGORY_VIDEO_DECODER` attribute, and running the MFT
  as a real **asynchronous** MFT (`AVS_MFT_ASYNC=1`, see below) - all with the same
  result.  The side by side logs also show where the two branches part: both post
  `MEEndOfPresentation`, run the clock to the end of the content and reach
  `topology=ended(400)` plus `MESessionCapabilitiesChanged`, but the H.264 control then
  posts `MESessionEnded` while the AVS branch instead moves the presentation clock to
  `state=stopped` (position back to 0) and stays there - the session stops presenting
  without calling the presentation finished.  With the control message names corrected against the SDK, the trace shows that
  the graph never sends `MFT_MESSAGE_NOTIFY_END_OF_STREAM` to the MFT: it sends
  `NOTIFY_BEGIN_STREAMING`, `NOTIFY_START_OF_STREAM`, `COMMAND_DRAIN` and
  `COMMAND_FLUSH` only.  The earlier lead on `MESessionStreamSinkFormatChanged` is **gone**: with our own MFT the
  H.264 control ends with `fmtchg=0`, so that event is a symptom of the system decoder
  renegotiating, not a precondition for ending.  `scripts\session-matrix.sh` prints the session matrix;
  check 11d pins the attribution result (it fails if that set of results drifts, so the
  tracked defect cannot quietly turn green); `plan\avs-mf-playback.md` keeps the KRs and
  the open hypotheses.
  `tmp\sm_*.log` are its logs.  Watch the fixtures for clearly longer than their
  duration: the session posts `MESessionEnded` only once its clock reached the end, so a
  20 s window on a 20.7 s clip is a false negative (the H.264 control showed exactly
  that).
  `IMFPMediaPlayer::GetPosition` fails with `0x80070057` in this SDK for native files
  too, so position cannot be used as a criterion.  The `IMFMediaSource` ->
  `IMFSourceReader` -> MFT path (checks 1-10) is unaffected.
* **What a host can use instead of `MESessionEnded` (measured, check 11n).**  "No
  `MESessionEnded`" is not the same as "the player cannot tell that playback finished".
  `bin\mfsession-test.exe -ts -grabber` now prints one line per run that names the signals and,
  critically, who stopped the clock:
  ```text
  eos-signals: end-of-presentation=2937 ms topology-ended=3039 ms clock-stopped-by-graph=3039 ms
               host-stop=8006 ms clock-max=3.031 s duration=not stated session-ended=no
  ```
  On two real AVS fixtures delivered in full to a real `IMFStreamSink` (26/26 raw `.avs`,
  19/19 AVS in MPEG-TS) the graph posts `MEEndOfPresentation`, then
  `MESessionTopologyStatus / MF_TOPOSTATUS_ENDED`, and then **stops its own presentation clock**
  (`IMFClock::GetState == MFCLOCK_STATE_STOPPED`, position reset to 0) - all within ~2 ms of each
  other, and ~5 s before the harness itself calls `IMFMediaSession::Stop()`.  So a host has three
  usable signals in a fixed order; a host that only listens for `MESessionEnded` will hang.
  The negative control is part of the check: the H.264-through-our-MFT cell under a sample
  grabber starts, delivers nothing and shows none of the three signals, so "all three columns
  present" cannot be satisfied by an empty or failed run.
  **Pinned gap on our side**: for an elementary `.avs`/`.dra` stream our presentation descriptor
  states no `MF_PD_DURATION` (a container does state it, e.g. 2.965 s), so a host that shows
  "position / duration" has no denominator for elementary streams.
* **End of playback has two measurement worlds, and they must not be mixed (integration note, check 11o).**
  The three signals above were measured with `bin\mfsession-test.exe -ts -grabber`, i.e. against a real
  `IMFStreamSink` that counts samples.  A player does not build that: it builds
  `MFCreateVideoRendererActivate` (the EVR), and there is **no sample counter under a renderer**.  The
  two worlds therefore have different observables, and `scripts\eos-signals-test.py` keeps them apart
  (`--sink grabber` | `--sink renderer`) rather than letting one assertion set certify the other:
  under the grabber the criterion is *every picture delivered* (26/26, 19/19) plus the signals; under a
  renderer there is no count to assert, so the criterion is `MESessionTopologySet` + `topology=ready(100)`
  + the session starting + **the presentation clock reaching the length of the media** (an end event can
  never make up for a clock that stopped early) + how the presentation ends.
  That the two really differ is measured, not assumed: the H.264 control that reaches `MESessionEnded`
  with a renderer delivers **zero** samples under a grabber and fails with `MF_E_UNSUPPORTED_FORMAT`
  (`0xC00D3E98`).
  **What is not claimed here**: the three signals have never been recorded under a real renderer.  The
  MFPlay graphs did play end-to-end in a session that had a compositor (`mfplay 11a/11b: resolved and
  played, clean shutdown`, recorded in `tmp\tests39.log` … `tmp\tests44.log`, 11:19–13:19 on the day
  these notes were written), but those runs predate the
  `eos-signals` instrumentation, and this shell is a *disconnected* session where the EVR answers
  `E_ACCESSDENIED` (`0x80070005`) for `ActivateObject`.  Closing that gap is one command in an
  interactive desktop session (console logon or RDP):
  ```text
  scripts\interactive-playback-check.cmd
  ```
  which records the session facts, the EVR probe, both MFPlay graphs and the renderer-world cells under
  `out\interactive\`, and exits 2 when the session still has no desktop.  The expected outcome is
  pre-registered (plan 16.3): ready + started + clock reaching the media length + the three signals for
  AVS, and `MESessionEnded = no` for AVS / `yes` for the system-path H.264 control.  If a real renderer
  behaves differently, that is a product finding and this section changes with it.  Measured on 2026-11-24,
  in a session where the EVR really activates (`probe-evr.exe` reports `ActivateObject -> 0x00000000`):
  both AVS cells behave exactly as pre-registered, twice in a row (`clock=3.07 s` / `3.03 s` against media
  of 3.0 s, `MEEndOfPresentation` + `topology=ended` + a stopped presentation clock, `MESessionEnded = no`).
  The H.264 control cell of that check is currently **not** a valid comparison: it points at a custom-source
  H.264 fixture, and the default build deliberately refuses H.264 unless `AVS_MFT_ALLOW_H264=1` is set, so
  the graph is never built (`MF_E_TOPO_CODEC_NOT_FOUND`).  The control has to be the system-path fixture
  (`samples\control_h264.mp4`, MF's own source and decoder, no component of ours in the graph); until that
  is fixed the check reports it red on purpose rather than re-pinning it to the refusal.

* **The optional GPU path reports what it did, and a GPU that fails falls back to the CPU (integration note, check 11p).**  `AVS_VK=1` (default off) lets the decoder reconstruct on the GPU; check 11k pins that the pictures are bit-identical to a direct libavcodec decode.  What a host can read, on the decoder's own stderr:  `avsvk: AVS_VK is set but no usable Vulkan backend` when no device can be used, and  `avsvk: the backend gave up (<which batch>); N recorded operation(s) were never executed (mc=... idct=... filter=...); the software kernels take over` when the backend stops working mid-stream.  The second line is printed once per process and nothing is attempted on the GPU afterwards - measured: 24 transforms in a run that gives up, against 402690 in a normal run - so the cost of a lost device stays bounded.  Measured behaviour (`scripts/gpu-fallback-test.py`): a backend that never comes up, and a backend that declines block after block, both produce byte-identical pictures (26/26 against an independent direct decode) with exit code 0, and the default path never mentions the backend at all.  **A backend that gives up in the middle of a stream also produces byte-identical pictures.**  The decoder keeps a record of every operation it handed over, with the parameters the software kernels need; when the backend drops work it was never going to execute, those operations are replayed by the software kernels before the GPU is given up on, and the give-up line now says so: `... were never executed (mc=... idct=... filter=...), replayed=N by the software kernels`.  A separate line, printed by any run that used the backend, reports the total (`avsvk: software replay after a give-up: N operation(s)`) so that "the mechanism only exists on the failure path" is checkable: it is `0` in every run where nothing failed, and equal to the outstanding work in every run that gave up (measured 1, 1, 1, 1, 1 and 5).  This holds at every tested failure point (`AVS_VK_FAIL_AFTER` = 0, 1, 2, 5, 30), on the 700-picture sample, and for two different queue shapes; what is *not* claimed is that the GPU path has no defect of its own, and check 11p now reports those separately so the fall-back can neither cause nor hide them: the reconstruction on `samples\CCTV-4.avs` differs from a direct decode in its last two pictures (2 of 700, delta 221), and the experimental `AVS_VK_DEFER_IDCT=1` shape is not bit-exact on its own and contains one submit failure that is not reported at all.  Those three are registered, not fixed.  Practical advice for an integrator: keep `AVS_VK` off unless you need it, and read `replayed=` as the signal that a lost GPU preserved the picture.  **Why this path is off by default is now a measured verdict, not caution.**  26 pictures at 720x576: the CPU path takes 442 ms and the GPU path 20124 ms, 44.8x slower.  Of that, the operations themselves are 2164 ms - already **4.9x slower than everything the CPU does for the same pictures** (entropy decode, prediction, residual, loop filter included) - and the remaining 17960 ms is the non-compute cost: about 11.9 GB of copies in and out of the arena plus one `submit`/`wait` pair per operation (153492 of them, ~100-115 us each, estimated; the two independent routes - `NOOP` time per submit and the `NO_MC` difference per operation - agree to the same order).  Because the *operations alone* lose by 4.9x, driving the transfers and the submissions to zero cannot make this path faster than the CPU, so no amount of batching or deferral changes the verdict: AVS reconstruction at 8x8/16x16 block granularity with tight dependencies is CPU-cache friendly and the GPU cannot amortise its scheduling at that size.  The path is kept because it is a **bit-exact reference implementation** (26/26 against an independent decode, pinned by check 11k), which is useful for validating DSP changes; it is not a performance option.  Integrators should note why these lines come from the decoder rather than from FFmpeg: an MF host cannot see FFmpeg's own diagnostics, because `FFmpegDecoder.cpp` installs an `av_log` callback that is a no-op unless the DLL is built with `AVS_MFT_TRACE_FFMPEG`, and even then it goes to the debugger instead of the log.
* **A real MPEG-TS file is resolved by content, not by extension (integration note).**  Registering our byte stream handler for an extension does not give us AVS-in-TS content.  Measured with the most permissive resolver flags (`MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE`): the real broadcast `video\P2016-8-14-20-20-301.ts` copied to `.tts` (registered under HKCU) **and** copied to `.avs` (registered under HKLM by this DLL) is both resolved by the system `MPEG2 Byte Stream Handler`, and our handler is never called (`AVS_MFT_TRACE=1` shows zero of our lines).  The same extension with content the sniffer does not recognise as TS (a truncated TS cut) *is* handed to us: `[avssrc] probe over 131072 byte(s): mpegts (score 100)`.  The bytes themselves are fine in our hands - forcing the handler in process describes that exact file as `video/AVS 720x576` plus `audio/DRA 48000` (check 11m).  Consequence: to play a broadcast `.ts` by double-click, an installer has to take over the `.ts` association from the system handler, or the product has to accept that only elementary AVS/DRA streams open that way; a host that creates its own byte stream can always select our handler explicitly.
* **A sequence header that arrives late costs the first pictures.** `bin\avsmft-test.exe
  samples\CCTV-4-short.avs ... -mft ... -fmt i420 -nosize` builds an input type without
  `MF_MT_FRAME_SIZE`, which is how a real stream often starts (the header is only parsed
  after the first access units).  The decoder then produces all 26 pictures
  (`picture out=` x26) but only 18 are handed on, and the 8 missing ones are the *first* 8
  of the presentation (delivery starts at 2.360 s while pictures exist from 2.040 s) -
  exactly `AVS_MFT_MAX_QUEUED_FRAMES`.  Not attributed yet (the output `SetOutputType`
  path deliberately keeps the queue when the change came from the bit stream, so the
  discard happens somewhere else); acceptance when fixed: `-nosize` hands on 26 pictures and
  the bytes match the normal path.

* **A media type this MFT accepts must also be listed by `GetInputAvailableType`.** The
  topology loader decides whether a transform can serve a stream by enumerating its input
  types, not by trying `SetInputType`: accepting a subtype in `IsInputSubtype()` alone
  makes the loader answer `MF_E_TOPO_CODEC_NOT_FOUND` (`0xC00D5212`).
* **`AVSMediaSource` publishes H.264 in MPEG-TS as `MFVideoFormat_H264_ES`**
  (`Data1=3F40F4F0`), not `MFVideoFormat_H264`; the system decoder is matched the same way.
  When a type is refused, `AVS_MFT_TRACE=1` now prints the rejected subtype and the state
  of the `AVS_MFT_ALLOW_H264` measurement gate.
* **Chroma row pitch is never the luma pitch for the planar formats - in the tools either.**
  On the MFT side the rule lives in one place (`AvsLayoutFor()`), but `bin\avsmft-test.exe`'s BMP
  dump kept its own copy and took the chroma pitch from `MF_MT_DEFAULT_STRIDE`: that read about
  twice past the end of the sample, and whether it trapped depended on the committed heap tail -
  about 6 crashes in 10 runs with **correct bytes, correct frame count and exit code 139**.
  Check 11e (`scripts\exit-code-test.sh`) now asserts the exit code over repeated runs, because a
  single run proves nothing here.  To attribute a fault like this to a function: build the tool
  with `/MAP` (already done for `avsmft-test.exe`) and run
  `python scripts\symres.py --map=bin\avsmft-test.map 6CBA0`.
* **Do not put `MF_MT_AVG_BIT_ERROR_RATE` on the decoder's output type.** Setting it
  (`VT_R4` through `IMFAttributes::SetItem`) makes the topology loader answer
  `MF_E_INVALIDTYPE` (`0xC00D36BD`) and the session cannot be started at all - the clock
  stays at 0.  It looked format related (`MESessionTopologySet status=0xC00D36BD`), but
  bisection showed that this single attribute is what broke every AVS / AVS2 / AVS3
  topology.  `MF_MT_COMPRESSED = FALSE` is fine and is kept.
* **Media Foundation requires strictly increasing output sample times.** Damaged or
  truncated streams really do carry pictures that share a time stamp (upstream FFmpeg
  reports the last three frames of `samples\CCTV-4-short-ts.avs` with the same PTS), and
  a sample scheduled in the past stops the presentation clock.  `FFmpegVideoDecoder` now
  keeps the newest time it has handed out plus the last real frame interval and bumps such
  pictures instead of emitting a duplicate.  Matching a decoded picture to a buffered
  sample time also has to erase *only* the matched entry: that queue is in feed (decode)
  order, so dropping everything before the match throws away the newest presentation times
  of a B picture stream.
* Sample delivery is demand driven: an access unit that is read while the *other* stream
  is being served goes into that stream's `m_Pending` backlog and is posted only when
  the consumer asks for it.  Posting a whole file worth of unrequested
  `MEMediaSample` events made the media pipeline stop requesting after ~24 samples, and
  the old cap silently dropped access units beyond 256.
* A media source has to post `MEEndOfPresentation` itself once every started stream has
  posted `MEEndOfStream` (`NotifyStreamEnded`), and each further `RequestSample` after
  the end has to be answered with another `MEEndOfStream`; without that the session
  never learns that the presentation finished.  Playback still does not report as ended
  (see above), but the source side is now complete.
* End of file and end of stream are handled in three places: the libavformat read
  callback returns `AVERROR_EOF` for a zero length byte stream read (returning 0 makes
  libavformat retry forever), each input stream posts `MEEndOfStream`, and both decoder
  MFTs keep drained pictures / audio blocks in `m_FlushFrames` when the output queue is
  full, so the last units of a stream are delivered instead of dropped (verified frame
  exact against upstream FFmpeg: 26/26 for a 3 s clip, 700/700 for a 30 s clip).
* Stream descriptors are referenced twice on purpose: once by `m_Streams`, once by the
  presentation descriptor.  Releasing either of them twice crashes the host during
  shutdown (this was the MFPlay crash).
* A hand-built topology needs **both** descriptors on a source-stream node:
  `MF_TOPONODE_SOURCE`, `MF_TOPONODE_PRESENTATION_DESCRIPTOR` *and*
  `MF_TOPONODE_STREAM_DESCRIPTOR`.  Setting only the stream one makes `SetTopology` answer
  `MF_E_TOPO_MISSING_PRESENTATION_DESCRIPTOR` (`0xC00D5217`).  A sink node whose sink was never
  activated must not be added at all: an output node holding an unactivated `IMFActivate` gets
  the whole topology refused, so one stream without a device takes the working streams down with
  it.  Releasing the session and the topology before `FreeLibrary` of the DLL that contains the
  MFT is not optional - the topology still points into code that has been unloaded.
* Seeking / `Start` at a non-zero position is implemented (the source seeks relative to
  its time base) but not yet verified by a test.
* AVS2 / AVS3 decode through `libdavs2` / `libuavs3d`; `scripts/build-deps.cmd` builds
  those libraries (add `10` for the 10 bit variants) and `scripts/build-ffmpeg.cmd`
  picks them up.  10 bit streams are kept 10 bit when the sink asks for `P010`; the
  output type list keeps the 8 bit formats first, so the default negotiation is still
  NV12.  `bin\avsmft-test.exe -fmt p010` compares bit exact against
  `ffmpeg -pix_fmt p010le`.
* Colour signalling (`MF_MT_VIDEO_PRIMARIES`, `MF_MT_TRANSFER_FUNCTION`,
  `MF_MT_VIDEO_NOMINAL_RANGE`, `MF_MT_PIXEL_ASPECT_RATIO`) is copied from the container
  and overridden by what the decoder reports per frame; `MF_MT_AUDIO_CHANNEL_MASK` is
  derived from the FFmpeg channel layout when it can be named.  Nothing is guessed:
  a stream that says nothing gets no attributes, which is why `MF_MT_YUV_MATRIX` and
  `MF_MT_BITS_PER_PIXEL` stay unset (no usable enum / constant in the local SDK).
* CPU decode only.  There is no standard DXVA2/D3D11VA/NVDEC path for AVS; a hardware
  path would need a custom decoder backend.
* `MF_MT_MPEG_SEQUENCE_HEADER` on the input type is passed to the decoder as
  `extradata`; AVS carries its sequence header in band, so it works without it.

* **The pictures at the end of a raw elementary stream now match an independent decoder,
  and the reason they did not is a timestamp contract, not a decoder bug.**
  `scripts/reference-order-test.py` decodes the same file with FFmpeg's own native `cavs`
  decoder (prebuilt n8.0 tree - none of our code in it) and compares *frame by frame*, so a
  wrong order and a wrong picture come out as different failures.  Measured state:

  * default configuration: 26/26 pictures of the 3 s sample and 700/700 of the 30 s sample
    equal the reference **in order**.  The decode canary of the default path is now the same
    value as the independent reference (`6c52e2bcd47d12ceee40532e1173eb0e`).
  * what was wrong: the wrapper reordered held pictures by their presentation time whenever
    all of them had one.  A raw elementary AVS stream does not state a unique time per picture
    - the 26 picture fixture is carried by 75 compressed samples with only 24 distinct times,
    and the pictures the decoder releases at end of stream report a time that was already used.
    A time stamp is a valid presentation key only if it is later than everything already handed
    out *and* unique inside the window being reordered; when it is not, the decoder's own order
    is kept (it is display order - measured against the reference on both fixtures) and the
    later monotonic repair still gives every picture a fresh, strictly increasing time.  With
    a real container the guard never fires: over an AVS-in-MPEG-TS fixture 0 reorder windows
    are degenerate, so B-frame reordering still happens by time stamp.
  * still open, and reported as such: with `AVS_AEC_ZERO=1` (single instance, not the shipped
    default) the 30 s sample differs in **content** at the very end - the reference's last
    picture is missing and one of ours is not in the reference.  And streams of fewer than 7
    pictures come out in the wrong order in the decoder itself (every picture is present, so
    the pictures are correct and only the order is wrong).  Pinned as regression items 11f
    (`EXPECT_RED=2`, those two assertions only) and 11h (every cut of 7 pictures or more must
    keep the reference order; shorter cuts are counted separately and capped).
  * to check order rather than counts, `bin\mfsession-test.exe <file> -grabber -dump out.yuv`
    writes the pictures the *graph* presented, in presentation order; counting delivered
    samples cannot see a rotated tail.
