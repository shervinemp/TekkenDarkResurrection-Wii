#!/bin/sh
# Builds FFmpeg's H.264 decoder (and nothing else) for the Wii, for the intro movie (runtime/hle/avc.c).
#
#   sh tools/build_ffmpeg.sh            download (if needed), configure, build, install
#
# Result: third_party/ffmpeg-ppc/{include,lib} (libavcodec.a, libavutil.a).  The Makefile links them when they exist
# (`make EXP_NOFFMPEG=1` builds the black-movie stub instead).  Nothing here is committed: third_party/ffmpeg-src,
# ffmpeg-build and ffmpeg-ppc are ignored by git.
#
# License: FFmpeg is LGPL-2.1+ as configured here (--disable-gpl --disable-nonfree; configure prints "License: LGPL version
# 2.1 or later").  See THIRD_PARTY.md.  The source is the unmodified release tarball.
#
# Needs devkitPPC (powerpc-eabi-gcc in PATH, DEVKITPRO set) and a POSIX shell with make, tar, xz, curl or wget and
# sha256sum; on Windows run it from Git Bash or devkitPro's MSYS2 with TMPDIR set to a POSIX path (configure fails with a
# backslash TMPDIR) and with MAKE pointing at a make that understands /c/... paths (devkitPro's msys2 make, e.g.
# MAKE=/c/devkitPro/msys2/usr/bin/make; the GnuWin32 make does not).
#
# OPTFLAGS (default -Os) is the optimisation level of FFmpeg's code.  The size matters: the program and the host thread
# stacks share MEM1, and the movie player's worker threads fail to start ("cannot start host thread") when less than about
# 4 MB of MEM1 is left.  The DOL grows by 0.85 MB with -Os (4104 KiB left, works), 1.1 MB with -O2 (3860 KiB left, fails)
# and 2.1 MB with -O3 (2936 KiB left, fails); see docs/HANDOVER.md, B1.
set -e

VERSION=7.1.1
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
URL=https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz
OPTFLAGS=${OPTFLAGS:--Os}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/third_party/ffmpeg-src
BLD=$ROOT/third_party/ffmpeg-build
PFX=$ROOT/third_party/ffmpeg-ppc
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
LIBOGC=$DEVKITPRO/libogc
export PATH="$DEVKITPRO/devkitPPC/bin:$PATH"
command -v powerpc-eabi-gcc >/dev/null || { echo "powerpc-eabi-gcc not found (devkitPPC)" >&2; exit 1; }

if [ ! -f "$SRC/configure" ]; then
    TAR=$ROOT/third_party/ffmpeg-$VERSION.tar.xz
    if [ ! -f "$TAR" ]; then
        mkdir -p "$ROOT/third_party"
        echo "downloading $URL"
        if command -v curl >/dev/null; then curl -fL -o "$TAR" "$URL"; else wget -O "$TAR" "$URL"; fi
    fi
    echo "$SHA256  $TAR" | sha256sum -c -
    mkdir -p "$SRC"
    tar -xJf "$TAR" -C "$SRC" --strip-components=1
fi

rm -rf "$BLD" "$PFX"
mkdir -p "$BLD"
cd "$BLD"

# The host compiler is only asked to compile test programs (nothing is built for the host with these options), and there
# is no working host gcc on a plain devkitPro install, so the cross compiler stands in for it.
MACH="-mrvl -mcpu=750 -meabi -mhard-float"
"$SRC/configure" \
    --prefix="$PFX" \
    --enable-cross-compile --target-os=none --arch=ppc --cpu=750 \
    --cross-prefix=powerpc-eabi- --cc=powerpc-eabi-gcc --ar=powerpc-eabi-ar --nm=powerpc-eabi-nm \
    --ranlib=powerpc-eabi-ranlib --strip=powerpc-eabi-strip \
    --host-cc=powerpc-eabi-gcc --host-cflags="$MACH -I$LIBOGC/include" --host-ld=powerpc-eabi-gcc \
    --host-ldflags="$MACH -L$LIBOGC/lib/wii" --host-extralibs="-logc -lm" \
    --optflags="$OPTFLAGS" \
    --extra-cflags="-DGEKKO $MACH -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -I$LIBOGC/include" \
    --extra-ldflags="$MACH -L$LIBOGC/lib/wii" --extra-libs="-logc -lm" \
    --disable-everything --enable-decoder=h264 --enable-parser=h264 \
    --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter \
    --disable-swscale --disable-swresample --disable-postproc \
    --disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-asm --disable-debug --disable-autodetect \
    --disable-gpl --disable-nonfree --enable-static --disable-shared
grep -q 'FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h || { echo "FFmpeg is not configured as LGPL" >&2; exit 1; }

MAKE=${MAKE:-make}
"$MAKE" -j"$(nproc 2>/dev/null || echo 4)"
"$MAKE" install

# The decoder's memory comes from the MEM2 pool, not the newlib heap (MEM1 has about 1 MB left): rename its references to
# the allocator, which runtime/hle/avc_mem.c provides.
for lib in libavutil.a libavcodec.a; do
    powerpc-eabi-objcopy --redefine-sym malloc=tkdr_ff_malloc --redefine-sym free=tkdr_ff_free \
        --redefine-sym realloc=tkdr_ff_realloc --redefine-sym memalign=tkdr_ff_memalign "$PFX/lib/$lib"
done
echo "FFmpeg $VERSION installed in $PFX"
