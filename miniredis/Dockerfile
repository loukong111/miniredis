FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        make && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DMINIREDIS_ENABLE_INTEGRATION_TESTS=OFF \
        -DMINIREDIS_BUILD_QT_CONSOLE=OFF && \
    cmake --build build -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl && \
    rm -rf /var/lib/apt/lists/* && \
    useradd --system --no-create-home --shell /usr/sbin/nologin miniredis && \
    mkdir -p /var/lib/miniredis && \
    chown -R miniredis:miniredis /var/lib/miniredis

COPY --from=builder /src/build/miniredis /usr/local/bin/miniredis

USER miniredis
EXPOSE 6366 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["miniredis"]
CMD ["--bind", "0.0.0.0", "--port", "6366", "--stats-bind", "0.0.0.0", "--stats-port", "8080", "--snapshot-file", "/var/lib/miniredis/snapshot_6366.dat"]
