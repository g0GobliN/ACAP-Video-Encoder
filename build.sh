#!/usr/bin/env bash
set -euo pipefail

ARCH="${1:-armv7hf}"
case "$ARCH" in armv7hf|aarch64) ;; *) echo "usage: build.sh <armv7hf|aarch64>" >&2; exit 2 ;; esac

ROOT="$(cd "$(dirname "$0")" && pwd)"
TAG="acap-video-encoder-build:$ARCH"
OUT="$ROOT/build/$ARCH"
mkdir -p "$OUT"
docker build --platform linux/amd64 --build-arg "ARCH=$ARCH" -t "$TAG" "$ROOT"
container="$(docker create --platform linux/amd64 "$TAG")"
trap 'docker rm -f "$container" >/dev/null' EXIT
docker cp "$container:/output/." "$OUT/"
echo "ACAP package written to $OUT"
