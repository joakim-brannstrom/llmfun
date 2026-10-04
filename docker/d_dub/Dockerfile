FROM ubuntu:resolute
LABEL authors="Joakim Brännström <joakim.brannstrom@gmx.com>"

RUN rm -rf /var/lib/apt/lists/*

RUN apt update && \
    DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" --no-install-recommends install \
        ca-certificates

RUN apt upgrade -y

RUN apt update && \
    DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" --no-install-recommends install \
        ca-certificates

RUN apt install -y --no-install-recommends \
        git \
        make cmake ninja-build \
        gcc g++ gdb \
        curl \
        xz-utils \
        libncurses-dev \
        libglfw3-dev xvfb mesa-utils pkg-config

        # vulkan-tools libvulkan-dev vulkan-validationlayers glslc spirv-headers pkg-config
WORKDIR /opt

ENV LDC_VERSION=1.42.0
RUN curl -L https://github.com/ldc-developers/ldc/releases/download/v${LDC_VERSION}/ldc2-${LDC_VERSION}-linux-x86_64.tar.xz \
    -o ldc.tar.xz \
 && tar -xf ldc.tar.xz \
 && mv ldc2-${LDC_VERSION}-linux-x86_64 /opt/ldc
ENV PATH="/opt/ldc/bin:$PATH"
ENV DC="ldc2"

RUN mkdir /opt/deps
COPY dub.sdl /opt/deps
RUN cd /opt/deps && dub fetch --cache=system
RUN cd /opt/deps && dub build --cache=system || true

RUN mkdir /workarea
WORKDIR /workarea
