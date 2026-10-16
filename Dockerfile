ARG ARCH=armv7hf
FROM axisecp/acap-native-sdk:12.11.0-${ARCH}-ubuntu24.04

ARG ARCH
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      curl git make gcc libc6-dev pkg-config nasm yasm xz-utils ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/app
COPY app/ ./
COPY build-ffmpeg.sh /opt/build-ffmpeg.sh
RUN bash /opt/build-ffmpeg.sh "${ARCH}" && \
    . /opt/axis/acapsdk/environment-setup* && \
    acap-build . -a source && \
    mkdir -p /output && cp ./*.eap ./*_LICENSE.txt /output/
