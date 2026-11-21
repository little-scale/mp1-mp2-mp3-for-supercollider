# Third-party notices

The codec implementation comes from little-scale’s MP1, MP2 and MP3 Max suite, version 2.0.0. The SuperCollider extension uses its portable codec sources, including the Layer I and II filter banks and the Layer III field packer. [Upstream source](https://github.com/little-scale/mp1-mp2-mp3-for-max/tree/v2.0.0)

The build uses the SuperCollider 3.14.1 plugin headers. Their copyright belongs to James McCartney and the SuperCollider contributors. The upstream distribution includes the GNU General Public License. [SuperCollider source](https://github.com/supercollider/supercollider/tree/Version-3.14.1) · [License text](SuperCollider-GPL.txt)

LAME 3.100 supplies the Layer III PCM encoder. The archive, modified API header and between-frame bitrate patch are in `source/codec/vendor`. The script `source/codec/build-lame-macos.sh` rebuilds the replaceable dynamic library. The upstream archive SHA-256 is `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`. The packer also uses Huffman tables from LAME’s `libmp3lame/tables.c`. The extracted header retains the original notice. [LAME](https://lame.sourceforge.io/) · [License text](LAME-LGPL.txt)

minimp3 by lieff and contributors supplies Layer III decoding and layout tables under CC0. The bundled header includes its original dedication. The local mixed-block table covers the 8 kHz layout used by this package. [minimp3](https://github.com/lieff/minimp3)
