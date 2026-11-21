#!/bin/sh
# Rebuild the replaceable LGPL encoder from the bundled source and patch.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dest=${1:?Specify the destination dylib}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
tar -xzf "$here/vendor/lame-3.100.tar.gz" -C "$scratch"
src="$scratch/lame-3.100"
(cd "$src" && patch -s -p1 < "$here/vendor/lame-frame-bitrate.patch")
for arch in arm64 x86_64; do
    build="$scratch/$arch"
    mkdir "$build"
    case "$arch" in arm64) cpu=aarch64 ;; *) cpu=x86_64 ;; esac
    (cd "$build" && CC=clang CFLAGS="-O3 -arch $arch -mmacosx-version-min=11.0" \
      LDFLAGS="-arch $arch -mmacosx-version-min=11.0" "$src/configure" \
      --host="$cpu-apple-darwin" --disable-frontend --disable-nasm \
      --enable-shared --disable-static && make -j4 -C mpglib && make -j4 -C libmp3lame)
done
lipo -create "$scratch/arm64/libmp3lame/.libs/libmp3lame.0.dylib" \
             "$scratch/x86_64/libmp3lame/.libs/libmp3lame.0.dylib" -output "$dest"
