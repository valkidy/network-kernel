# Runtime image for the dedicated server (linux/amd64).
#
# Built by tools/docker/build_image.sh, whose build context is a staging
# directory, not the repository:
#   bin/server        //app:app -c opt, stripped
#   data/bundle.zip   gameplay catalog bundle; every asset the server loads
#                     (catalog, navmesh, collision scene) comes out of it
#   entrypoint.sh     NK_* environment -> server flags
#   lib/              only if the binary links Bazel-built shared libraries
#
# Same base as builder.Dockerfile: the binary needs that glibc and libstdc++.
FROM ubuntu:24.04

ARG NK_IMAGE_VERSION=unknown
LABEL org.opencontainers.image.title="network-kernel dedicated server" \
      org.opencontainers.image.version="${NK_IMAGE_VERSION}"

# iproute2 is for tools/docker/netem.sh, which runs `tc` as root through
# `docker exec` to add delay/loss on the container's interface. The server
# itself never uses it.
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends iproute2 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin nk

COPY --chmod=0755 . /opt/nk/

ENV LD_LIBRARY_PATH=/opt/nk/lib \
    NK_PORT=7801 \
    NK_MAX_PLAYERS=4 \
    NK_NETCODE=standard

USER nk
EXPOSE 7801/udp
# The server is exec'd as PID 1 and installs its own SIGTERM handler, so
# `docker stop` reaches it directly; no init process is needed.
ENTRYPOINT ["/opt/nk/entrypoint.sh"]
