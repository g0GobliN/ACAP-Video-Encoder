#!/usr/bin/env bash
set -euo pipefail

ARCH="${1:?expected armv7hf or aarch64}"
case "$ARCH" in
  armv7hf) FF_ARCH=arm; HOST=arm-linux-gnueabihf ;;
  aarch64) FF_ARCH=aarch64; HOST=aarch64-linux-gnu ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 2 ;;
esac

. /opt/axis/acapsdk/environment-setup*
CC_BIN="${CC%% *}"
CROSS_PREFIX="${CC_BIN%gcc}"
BUILD="/tmp/encoder-build"
SRC="$BUILD/src"
PREFIX="$BUILD/prefix"
mkdir -p "$SRC" "$PREFIX" /opt/app/source

FFMPEG_VERSION=8.1.3
FFMPEG_SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
X264_COMMIT=31e19f92f00c7003fa115047ce50978bc98c3a0d
curl -fSL --retry 5 --retry-all-errors --retry-delay 2 \
  "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" -o "$BUILD/ffmpeg.tar.xz"
echo "$FFMPEG_SHA256  $BUILD/ffmpeg.tar.xz" | sha256sum -c -
tar -xf "$BUILD/ffmpeg.tar.xz" -C "$SRC"
mkdir -p "$SRC/x264"
git -C "$SRC/x264" init
git -C "$SRC/x264" remote add origin https://github.com/mirror/x264.git
git -C "$SRC/x264" fetch --depth 1 origin "$X264_COMMIT"
git -C "$SRC/x264" checkout --detach FETCH_HEAD
cp -a "$SRC/ffmpeg-${FFMPEG_VERSION}" /opt/app/source/ffmpeg
cp -a "$SRC/x264" /opt/app/source/x264
rm -rf /opt/app/source/x264/.git
printf 'FFmpeg %s\nx264 %s\n' "$FFMPEG_VERSION" "$X264_COMMIT" > /opt/app/source/VERSIONS.txt

cd "$SRC/x264"
CC="$CC" ./configure --host="$HOST" --cross-prefix="$CROSS_PREFIX" \
  --prefix="$PREFIX" --enable-static --enable-pic --disable-cli --disable-opencl
make -j"$(nproc)" AS="$CC"
make install

cd "$SRC/ffmpeg-${FFMPEG_VERSION}"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
# The SDK compiler carries -L$SDKTARGETSYSROOT/usr/lib, and that sysroot ships an older shared
# FFmpeg. Drop the flag so the ffmpeg link uses the static libraries built here.
FF_CC="${CC//-L$SDKTARGETSYSROOT\/usr\/lib/}"
./configure \
  --prefix=/usr --arch="$FF_ARCH" --target-os=linux --enable-cross-compile \
  --cross-prefix="$CROSS_PREFIX" --cc="$FF_CC" --host-cc=gcc --pkg-config=pkg-config --ar="$AR" --nm="$NM" \
  --strip="$STRIP" --sysroot="$SDKTARGETSYSROOT" \
  --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
  --disable-autodetect --disable-doc --disable-debug --disable-network \
  --disable-ffplay --disable-ffprobe --disable-shared --enable-static \
  --disable-encoders --disable-decoders --disable-demuxers --disable-muxers \
  --enable-gpl --enable-libx264 --enable-ffmpeg --enable-avfilter --enable-swscale \
  --enable-demuxer=concat --enable-demuxer=image2 --enable-decoder=mjpeg \
  --enable-parser=mjpeg --enable-encoder=libx264 --enable-muxer=mp4 --enable-protocol=file \
  --enable-protocol=pipe --enable-filter=null --enable-filter=format --enable-filter=scale \
  || { tail -n 100 ffbuild/config.log; exit 1; }
make -j"$(nproc)"
make install DESTDIR="$BUILD/root"
mkdir -p /opt/app/lib
install -m 0755 "$BUILD/root/usr/bin/ffmpeg" /opt/app/lib/ffmpeg
cp "$SRC/ffmpeg-${FFMPEG_VERSION}/COPYING.GPLv2" /opt/app/LICENSE
cp "$SRC/x264/COPYING" /opt/app/source/x264-COPYING
