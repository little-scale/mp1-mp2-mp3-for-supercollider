#!/bin/sh
# Build both server variants with the SDK matching the installed SuperCollider.
set -eu
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
sdk=${1:?Supply the absolute path to the SuperCollider sources}
out=${2:-"$here/../plugins"}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
lib="$out/libmp3lame.0.dylib"
if [ -n "${3:-}" ]; then cp "$3" "$tmp/lame.dylib"; else sh "$here/codec/build-lame-macos.sh" "$tmp/lame.dylib"; fi
install_name_tool -id @loader_path/libmp3lame.0.dylib "$tmp/lame.dylib"
codesign --force --sign - "$tmp/lame.dylib"
cp "$tmp/lame.dylib" "$lib"
for arch in arm64 x86_64; do
    obj="$tmp/$arch"; mkdir "$obj"
    for name in layer3 pack spectral_rate field_file layers12/subband layers12/filter layers12/convert layers12/reader layers12/layer1; do
        case "$name" in layers12/*) family=-DMPEG_AUDIO_LAYERS_12;; *) family=;; esac
        xcrun clang -arch "$arch" -mmacosx-version-min=11.0 -std=c11 -O3 -fvisibility=hidden $family -c "$here/codec/$name.c" -o "$obj/${name##*/}.o"
    done
    for file in Codec Service; do
        xcrun clang++ -arch "$arch" -mmacosx-version-min=11.0 -std=c++17 -O3 -fvisibility=hidden -c "$here/$file.cpp" -o "$obj/$file.o"
    done
    for variant in MPEGAudio MPEGAudio_supernova; do
        case "$variant" in *_supernova) defs=-DSUPERNOVA;; *) defs=;; esac
        xcrun clang++ -arch "$arch" -mmacosx-version-min=11.0 -std=c++17 -O3 -fvisibility=hidden $defs \
          -I"$sdk/include/plugin_interface" -I"$sdk/include/common" -I"$sdk/include/server" \
          -bundle "$here/MPEGUGens.cpp" "$obj"/*.o "$lib" -o "$tmp/$variant.$arch"
    done
done
for variant in MPEGAudio MPEGAudio_supernova; do
    lipo -create "$tmp/$variant.arm64" "$tmp/$variant.x86_64" -output "$out/$variant.scx"
    codesign --force --sign - "$out/$variant.scx"
done
