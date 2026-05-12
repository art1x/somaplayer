#!/usr/bin/env bash
# Cross-compile mpg123 for tg5040 (aarch64 Cortex-A53)
# Must run inside the tg5040-toolchain Docker container.
# Produces: build/third_party/tg5040/mpg123/bin/mpg123
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT="$REPO_ROOT/build/third_party"
SOURCES_DIR="$BUILD_ROOT/sources"
PLATFORM="tg5040"
PLATFORM_ROOT="$BUILD_ROOT/$PLATFORM"
TRIPLET="aarch64-nextui-linux-gnu"
SYSROOT="/opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

MPG123_VERSION="1.32.9"
MPG123_ARCHIVE="mpg123-${MPG123_VERSION}.tar.bz2"
MPG123_URL="https://downloads.sourceforge.net/mpg123/${MPG123_ARCHIVE}"
MPG123_SHA256="03b61e4004e960bacf2acdada03ed94d376e6aab27a601447bd4908d8407b291"

INSTALL_DIR="$PLATFORM_ROOT/mpg123"
STAMP="$INSTALL_DIR/.stamp-mpg123-${MPG123_VERSION}"

if [[ -f "$STAMP" ]]; then
    echo "[mpg123] already built (stamp: $STAMP)"
    exit 0
fi

mkdir -p "$SOURCES_DIR" "$INSTALL_DIR/bin"

# ── Download ────────────────────────────────────────────────────────────────
if [[ ! -f "$SOURCES_DIR/$MPG123_ARCHIVE" ]]; then
    echo "[mpg123] downloading $MPG123_ARCHIVE …"
    wget -q --show-progress -O "$SOURCES_DIR/$MPG123_ARCHIVE" "$MPG123_URL"
fi

actual_sha="$(sha256sum "$SOURCES_DIR/$MPG123_ARCHIVE" | awk '{print $1}')"
if [[ "$actual_sha" != "$MPG123_SHA256" ]]; then
    echo "[mpg123] checksum mismatch for $MPG123_ARCHIVE" >&2
    echo "  expected: $MPG123_SHA256" >&2
    echo "  actual:   $actual_sha" >&2
    exit 1
fi

# ── Extract ─────────────────────────────────────────────────────────────────
SRC_DIR="$PLATFORM_ROOT/mpg123/src"
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR"
echo "[mpg123] extracting …"
python3 -c "import sys,bz2; sys.stdout.buffer.write(bz2.open(sys.argv[1]).read())" \
    "$SOURCES_DIR/$MPG123_ARCHIVE" | tar --strip-components=1 -xf - -C "$SRC_DIR"

# ── Configure ───────────────────────────────────────────────────────────────
cd "$SRC_DIR"
echo "[mpg123] configuring for $PLATFORM …"
CC="${TRIPLET}-gcc" \
AR="${TRIPLET}-ar" \
RANLIB="${TRIPLET}-ranlib" \
LD="${TRIPLET}-ld" \
STRIP="${TRIPLET}-strip" \
CFLAGS="-O2 -mcpu=cortex-a53 -mtune=cortex-a53" \
CPPFLAGS="-I${SYSROOT}/include" \
LDFLAGS="-L${SYSROOT}/lib -Wl,-rpath-link,${SYSROOT}/lib" \
LIBS="-lasound -lm -lpthread" \
PKG_CONFIG=false \
./configure \
    --build=x86_64-pc-linux-gnu \
    --host="${TRIPLET}" \
    --prefix="${INSTALL_DIR}" \
    --with-cpu=aarch64 \
    --with-audio=alsa \
    --disable-shared \
    --enable-static \
    --quiet

# ── Build ────────────────────────────────────────────────────────────────────
echo "[mpg123] building (${JOBS} jobs) …"
make -j"${JOBS}" --quiet

# Install only the binary (skip libs/headers we don't need)
mkdir -p "${INSTALL_DIR}/bin"
cp src/mpg123 "${INSTALL_DIR}/bin/mpg123"
"${TRIPLET}-strip" "${INSTALL_DIR}/bin/mpg123"

touch "$STAMP"
echo "[mpg123] done → ${INSTALL_DIR}/bin/mpg123"
