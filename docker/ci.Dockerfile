# Linux build and test image for Sheepdog (developer/CI image, never shipped).
# Debian 13: GCC 14, Clang 19, Qt 6.8 (the minimum Qt the code must build with).
FROM debian:13

ARG VCPKG_COMMIT=9a5dfef5d1a0438a7d24856a85bd60f309d355e9
ARG UID=1000
ARG GID=1000

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ gdb \
        clang clang-tidy libclang-rt-dev llvm \
        cmake ninja-build pkg-config \
        git ca-certificates curl zip unzip tar python3 \
        qt6-base-dev qt6-base-dev-tools libgl-dev \
        fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev \
    && mkdir -p /opt/vcpkg /build /vcpkg-cache /mnt/xdev \
    && chown dev:dev /opt/vcpkg /build /vcpkg-cache /mnt/xdev

COPY docker/run-preset.sh /usr/local/bin/run-preset
RUN chmod 755 /usr/local/bin/run-preset

USER dev
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout -q ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache \
    QT_QPA_PLATFORM=offscreen \
    CTEST_OUTPUT_ON_FAILURE=1

WORKDIR /src
