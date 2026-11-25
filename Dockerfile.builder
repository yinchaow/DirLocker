FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive \
    GOPATH=/root/go \
    GOPROXY=https://proxy.golang.org,https://goproxy.cn,direct \
    PATH=/usr/local/go/bin:/root/go/bin:/usr/local/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    curl \
    git \
    xz-utils \
    zip \
    unzip \
    qtbase5-dev \
    libqt5x11extras5-dev \
    libx11-dev \
    libxss-dev \
    libpeony-dev \
    libgtk-3-dev \
    libwebkit2gtk-4.0-dev \
    mingw-w64 \
    nsis \
    dpkg-dev \
    fakeroot \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install Go 1.27.4
ARG GO_VERSION=1.27.4
RUN curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" -o /tmp/go.tar.gz \
    && tar -C /usr/local -xzf /tmp/go.tar.gz \
    && rm -f /tmp/go.tar.gz

# Install Node.js 20.18.0
ARG NODE_VERSION=20.18.0
RUN curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" -o /tmp/node.tar.xz \
    && mkdir -p /usr/local/node \
    && tar -C /usr/local/node --strip-components=1 -xJf /tmp/node.tar.xz \
    && rm -f /tmp/node.tar.xz

# Install Wails v2 CLI
RUN go install github.com/wailsapp/wails/v2/cmd/wails@v2.17.1

WORKDIR /workspace
