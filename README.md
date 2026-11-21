# MP1, MP2 and MP3 for SuperCollider

Patch oscillators into an MPEG decoder, hold one band of a recording, or write edited coefficients directly to an MP1, MP2 or MP3 file. The extension ports all 20 objects in the Max suite. Every band is an ordinary SuperCollider signal, with 32 lanes per channel and independent left and right banks in stereo. [Max edition](https://github.com/little-scale/mp1-mp2-mp3-for-max)

**[Download the macOS universal extension](https://github.com/little-scale/mp1-mp2-mp3-for-supercollider/releases/latest/download/mp1-mp2-mp3-for-supercollider-macos-universal.zip)**

## Install

1. Evaluate `Platform.userExtensionDir.openOS` in SuperCollider. If the directory is missing, create it with `File.mkdir(Platform.userExtensionDir)`.
2. Unzip the release and copy the entire **MPEG Audio for SuperCollider** folder into that directory. Keep both server plugins and the LAME library together.
3. Recompile the class library. Before booting your server, evaluate the following. For a running server, use `s.reboot` after setting the option.

```supercollider
s.options.numWireBufs = 1024;
s.boot;
```

The release includes Apple silicon and Intel binaries for `scsynth` and `supernova`, built against SuperCollider 3.14.1. Search the SuperCollider help browser for **MPEG Audio** or any object name. Start with [Oscillators and holds](Examples/Oscillators-and-holds.scd) and [Edit a compressed file](Examples/Edit-a-compressed-file.scd).

## A decoder as an instrument

Evaluate this after the server boots. Four independent oscillators write quantized MP3 coefficients, and the other 28 bands start silent. Changing the mask holds the second band while its neighbors continue moving.

```supercollider
(
~voice = { |hold=0, kbps=0|
    var words = SinOsc.ar([1.7, 2.9, 4.1, 7.3]) * [14, 10, 7, 3];
    var frame = MPEGFrame.mp3(words, gain: 171);
    var mask = [0, hold] ++ (0 ! 30);
    var sound = MP3Decode.ar(frame, bitrate: kbps, holds: mask);
    Limiter.ar(LeakDC.ar(sound), 0.2) ! 2
}.play;
)
~voice.set(\hold, 1);
~voice.set(\hold, 0, \kbps, 64);
~voice.free;
```

## The objects

| Role | Layer I | Layer II | Layer III |
| --- | --- | --- | --- |
| PCM to editable fields and encoded bytes | `MP1Encode` | `MP2Encode` | `MP3Encode` |
| Fields or bytes to PCM | `MP1Decode` | `MP2Decode` | `MP3Decode` |
| Fit fields, stream bytes and optionally write a file | `MP1Pack` | `MP2Pack` | `MP3Pack` |
| Read a compressed file into fields | `MP1Unpack` | `MP2Unpack` | `MP3Unpack` |
| Recall a different frame age for each band | `MP1Frames` | `MP2Frames` | `MP3Frames` |
| Generate or follow frame and row timing | `MP1Clock` | `MP2Clock` | `MP3Clock` |

`MP1Bands.ar` is the parallel Layer I instrument. `MP1Decode.serial` exposes the original serial instrument, also available as `MP1Serial.ar`. `MP3Short.ar` applies three independent dB trims to short windows.

## Signals, settings and transport

- **Audio rate.** Words, allocations, scalefactors, attenuation, gains, windows, masks, markers, bytes, status and PCM can be patched sample by sample. All UGens run at audio rate. A layer reads its fields at the row, group or granule where the format uses them. A 32-value array represents a complete mono bank. A 64-value array orders the 32 left lanes before the 32 right lanes. A single value broadcasts. A 32-lane bank on a stereo input drives both channels.
- **Live settings.** Supply a number, a named synth control, a `.kr` signal or an `.ar` signal. `bitrate`, `freeze`, band `holds` and memory ages latch on frame boundaries. A hold value of 1 captures the previous band and 0 follows live input. Whole-frame freeze includes level. In field mode, `holdMode: \coeff` leaves the live Layer I/II scale or Layer III global gain in play. `\level` preserves the held level as well. In MP3 byte mode the held spectrum already includes its level. Dynamic hold modes are 0 and 1.
- **Fixed when building the SynthDef.** Channel count, a reader or writer’s server-side path, the number of incoming byte lanes, and history capacity determine the graph. Each new file path needs a new SynthDef or function graph. Open a new take or replay a file by moving `run` from 0 to 1. A writer finishes on the falling edge. Omitting the writer’s path still produces fields and bytes.
- **The frame object.** `MPEGFrame.mp1`, `.mp2` and `.mp3` create oscillator-driven frames. Encoders and readers return an `MPEGFrame` carrying the same signals. `f.withWords(newBank)` preserves timing and other fields. `f.bytes` from an encoder, reader or packer carries eight consecutive bytes per host sample in lane order, with −1 in unused lanes. An edited field bank takes effect when it reaches a decoder or packer. A packer’s field outputs carry the edit before bitrate fitting. Its bytes and optional `.audio` monitor reflect the fitted result.
- **Timing and status.** A marker of −2 selects the internal clock, −1 means idle and 0 holds the current row. Positive markers count from 1. `MP1Clock.ar(f)` and its siblings return the zero-based row, frame pulse, group or granule pulse, and marker. `f.codecRate` and `f.bitrate` read the stream format. Sources and packers expose `.state`, `.frames` and `.drops`. States are 0 idle, 1 active, 2 completed and −1 error. `f.running` can gate the writer directly from the reader through EOF. `MP3Decode.arInfo(f)` and `.bytesInfo(...)` return PCM plus the same status signals. Use `A2K.kr` when sending meters or status to the language.
- **File editing.** Each unpacker reads an indexed MPEG elementary stream. Layer III resolves the bit reservoir and converts joint-stereo material to editable left and right spectra. Repacking can change those spectra through quantization and fitting. At native rate the MP3 reader also exposes the original audio-frame bytes. The writer produces a fresh elementary stream with complete, budgeted frames. Tags and container metadata are outside that stream.

## Rates and bit budgets

| Layer | Supported codec sample rates (kHz) | Lowest selectable bitrate |
| --- | --- | --- |
| MP1 | 16, 22.05, 24, 32, 44.1, 48 | 32 kb/s |
| MP2 | 16, 22.05, 24, 32, 44.1, 48 | 8 kb/s at 16–24 kHz. At 32–48 kHz the minimum is 32 kb/s mono or 64 kb/s stereo. |
| MP3 | 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 | 8 kb/s at 8–24 kHz, or 32 kb/s at 32–48 kHz. |

A positive `bitrate` selects the nearest supported header rate for the current layer, sample rate and channel layout. It can change during a take. In field decoders, 0 synthesizes the supplied fields directly and a positive value auditions the fitted result. A byte stream supplies its own bitrate. Free-format MPEG streams are outside the reader’s supported formats.

`downsample: 2` selects half the native codec clock when that rate exists. Divisors run from 1 to 6 and stay in the same sample-rate family. On a 96 kHz server, the base codec rate is 48 kHz. MP3 can then use 24, 16, 12 or 8 kHz. PCM input and output follow the server rate, including fractional ratios. A linked field stream carries its original clock, so a divisor shared by multiple objects applies once. The file reader samples its divisor at start. A change to a take’s effective rate or channel count finalizes the file. Toggle `run` to begin the next take.

Encoding, fitting, file access and synthesis of complete codec frames run on per-instance workers in live servers. Frame buffering introduces latency. History and long stereo signal graphs consume additional memory and wires. The offline server processes frame jobs synchronously for repeatable renders.

## Build and test

On macOS, install the Command Line Tools and extract the matching [SuperCollider source release](https://github.com/supercollider/supercollider/releases/tag/Version-3.14.1). The build script compiles both universal plugins and rebuilds the bundled, replaceable LAME library from source.

```sh
sh source/build-macos.sh /absolute/path/to/supercollider
python3 tests/verify.py --live
```

The verification script needs the SuperCollider application, Python with NumPy, FFmpeg and ffprobe. Set `SC_APP` if the application is elsewhere. Its live tests boot isolated local servers with silent output and use temporary files. The tests cover fitted-decoder preview, band and full-frame holds, frame memory, changing bitrates, native and fractional host clocks, and live recording on both servers. Unedited native-rate file passes reproduced the same decoded samples for all three test streams. An independently encoded mid/side MP3 exercised VBR and the bit reservoir. Compiled plugins are distributed in GitHub Releases. [Third-party notices](licenses/Third-party.md)
