# Linux x86_64 build environment for the dedicated server.
#
# Holds the toolchain only; the repository is bind-mounted at /src by
# tools/docker/build_linux.sh, and Bazel's cache lives in a named volume so
# repeated builds stay incremental.
#
# perl + make are for @openssl_linux, whose genrule runs OpenSSL's own
# Configure and make (third_party/openssl_linux.BUILD).
FROM ubuntu:24.04

ARG BAZELISK_VERSION=v1.20.0

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        git \
        patch \
        perl \
        python3 \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-amd64" \
    && chmod +x /usr/local/bin/bazel

# The bind-mounted checkout is owned by the host user; tools/workspace_status.sh
# runs git inside it on every build.
RUN git config --system --add safe.directory '*'

WORKDIR /src
