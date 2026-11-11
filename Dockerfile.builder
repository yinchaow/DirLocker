FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    qtbase5-dev \
    libqt5x11extras5-dev \
    libx11-dev \
    libxss-dev \
    libpeony-dev \
    mingw-w64 \
    nsis \
    dpkg-dev \
    fakeroot \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

