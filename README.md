# ACAP Video Encoder

An on-camera video encoder for Axis cameras. It takes a JPEG sequence that already lives on the
camera's SD card and turns it into an H.264 MP4 without moving a single frame off the device. The
program is written in C and packaged as an ACAP application, with FFmpeg and x264 cross-compiled
into the same package.

## The problem

Encoding image sequences into video on a resource-constrained device presents a different set of
challenges from doing the same work on a workstation. Moving thousands of JPEGs off the device
adds unnecessary data transfer, while encoding locally requires careful management of CPU usage,
memory, and output integrity.

This project explores on-device H.264 encoding for Axis cameras, converting JPEG sequences stored
on the camera's SD card into MP4 video without transferring the source frames to another machine.

## How it works

The encoder is a headless C application packaged as an Axis ACAP application. It exposes an
authenticated FastCGI endpoint for starting and managing encoding jobs.

A request identifies the input frame sequence and output location. The encoder validates the
inputs, launches FFmpeg as a background process, and exposes job progress through status files.
Cancellation is supported, and the final MP4 is published only after FFmpeg completes successfully.

This keeps the encoding workflow asynchronous while protecting the output from being mistaken for
a completed video when an encode fails.

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
