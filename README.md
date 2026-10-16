# ACAP Video Encoder

An on-camera video encoder for Axis cameras. It takes a JPEG sequence that already lives on the
camera's SD card and turns it into an H.264 MP4 without moving a single frame off the device. The
program is written in C and packaged as an ACAP application, with FFmpeg and x264 cross-compiled
into the same package.

## The problem

Timelapse-style capture leaves thousands of JPEGs on the camera. Copying every frame to a
workstation to encode it spends most of its time moving data that never needed to leave the
device; encoding on camera keeps frames local and makes the video a side effect of the capture.

The trade is real: the camera has no hardware video encoder an app can feed photos into, so the
encode runs on CPU and the result is larger than a carefully tuned offline encode. The engineering
problem is making that trade predictable on a constrained, unattended device.

## How it works

A companion capture workflow on the same camera writes an ffconcat frame list to the SD card and
POSTs a form-encoded request to a single authenticated FastCGI endpoint (no UI, no sessions). The
encoder validates, forks a detached background job, and answers `202 Accepted`; the job execs
FFmpeg and reports progress through side files next to the output. Cancellation signals that job.

## Architecture

```
companion workflow         encoder (FastCGI)            background job
     │  POST start ──────────▶│ validate paths, frames,    │
     │                        │ output folder; take lock   │
     │◀──── 202 Accepted ─────│ fork ─────────────────────▶│ exec FFmpeg
     │                        │                            │  .part .progress .log .pid
     │  poll .progress/.done  │                            │  ok: rename .part → <job>.mp4
     │◀───────────────────────┴────────────────────────────│  then write .done (exit=N)
```

## Implementation highlights

- **One C file**, built with `-Wall -Wextra -Werror`, speaking FastCGI through libfcgi.
- **Validation before side effects**: every path is resolved with `realpath` and required to sit
  under the SD-card root; frame count, frame durations, and the output filename character set are
  checked before anything is spawned, and each rejection says which check failed.
- **Asynchronous by construction**: the HTTP request never waits for the encode. A file lock keeps
  it to one encode at a time, and the finished MP4 only takes its final name when FFmpeg exits
  cleanly, so a file called `<job>.mp4` is always a complete film.
- **Polite on a shared device**: the encode runs at lower CPU priority with a maximum OOM-score
  adjustment and three x264 threads, leaving a core and first claim on memory for frame capture.
- **Exact frame timing**: `setpts=N/(fps*TB)` places frame N at exactly N/fps — the manifest's
  rounded durations made neighbouring frames share a timestamp and dropped about one frame in six.
- **A purpose-built FFmpeg**: cross-compiled from a pinned, SHA-256-verified release with only the
  concat and image2 demuxers, MJPEG decoder, libx264 encoder, MP4 muxer, and scale filter enabled;
  networking and everything else is compiled out.

## Build

Prerequisites: Docker, plus network access to fetch the pinned FFmpeg and x264 sources.

```bash
bash build.sh armv7hf    # 32-bit ARM cameras
bash build.sh aarch64    # 64-bit ARM cameras
```

Each command builds inside the Axis ACAP Native SDK container image, cross-compiles x264 and
FFmpeg, and writes `build/<arch>/<package>.eap` plus its license file. The GitHub Actions workflow
runs both architectures on every push and pull request that touches the build inputs.

## What has actually been tested

- Both architectures have been built locally through `build.sh`; the CI workflow runs the same
  build commands on push and pull request.
- On-device behavior — frame timing, memory ceilings, priority and lock handling — was measured on
  real camera hardware during development.
- There is no automated test suite. The interesting failure modes (SD-card permissions, memory
  pressure on the device) need the hardware, so a green build proves the package compiles, not that
  it runs on a camera.

## Licensing

The build enables GPL components: FFmpeg and x264 are compiled into the package, FFmpeg's GPLv2
text ships as the package license (`app/LICENSE`), and the build output includes the corresponding
source for both. Third-party sources are downloaded at build time, not vendored.
