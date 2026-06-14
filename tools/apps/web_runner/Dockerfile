# Emscripten toolchain image for building the SeedSigner web_runner, extended
# with ccache so repeat builds (LVGL is hundreds of .c files) are fast.
#
# Built automatically by build.sh as the local tag `seedsigner-emsdk`. ccache is
# enabled via EM_COMPILER_WRAPPER=ccache at run time (Emscripten's supported way
# to cache the underlying clang invocations); the cache dir is bind-mounted so it
# persists across container runs.
ARG EMSDK_TAG=latest
FROM emscripten/emsdk:${EMSDK_TAG}

RUN apt-get update \
    && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/*
