// A language-side collection of ordinary audio-rate wires in a SynthDef.
MPEGFrame {
    var <layer, <channels, <>words, <>allocation, <>scale, <>attenuation, <>gain, <>window;
    var <>marker, <>format, <>bytes, <>audio, <>state, <>frames, <>drops;

    *bank { |value, count, fallback=0|
        var lanes = (value ? fallback).asArray.flat;
        if(lanes.size == 1) { ^Array.fill(count, { lanes.first }) };
        if((count == 64) and: { lanes.size == 32 }) { ^lanes ++ lanes };
        if(lanes.size > count) { Error("Too many MPEG lanes (% maximum)".format(count)).throw };
        ^lanes.extend(count, fallback)
    }

    *make { |layer, channels, words, first, second, window, marker, format|
        ^super.new.init(layer, channels, words, first, second, window, marker, format)
    }

    init { |l, ch, newWords, first, second, win, mark, meta|
        if([1, 2, 3].includes(l).not or: { [1, 2].includes(ch).not }) {
            Error("Invalid MPEG layer or channel count").throw
        };
        layer = l;
        channels = ch;
        words = this.class.bank(newWords, ch * 32);
        if(l == 3) {
            attenuation = this.class.bank(first, ch * 32);
            gain = this.class.bank(second, ch, 160);
            window = this.class.bank(win, ch)
        } {
            allocation = this.class.bank(first, ch * 32, 5);
            scale = this.class.bank(second, ch * 32, 18)
        };
        marker = mark;
        format = meta ? [0, 0, ch, 0, 32, -1, 0];
        if(format.size != 7) { Error("MPEG format requires seven lanes").throw };
        ^this
    }

    *mp1 { |words=0, allocation=5, scale=18, channels=1, marker= -2, format|
        ^this.make(1, channels, words, allocation, scale, nil, marker, format)
    }
    *mp2 { |words=0, allocation=5, scale=18, channels=1, marker= -2, format|
        ^this.make(2, channels, words, allocation, scale, nil, marker, format)
    }
    *mp3 { |words=0, attenuation=0, gain=160, window=0, channels=1, marker= -2, format|
        ^this.make(3, channels, words, attenuation, gain, window, marker, format)
    }

    withWords { |values|
        var edited = this.copy;
        edited.words = this.class.bank(values, channels * 32);
        edited.bytes = nil;
        edited.audio = nil;
        ^edited
    }

    wires { |expectedLayer|
        var other, n = channels * 32;
        if(layer != expectedLayer) { Error("Connect the matching MPEG layer").throw };
        other = if(layer == 3) {
            this.class.bank(attenuation, n) ++ this.class.bank(gain, channels, 160)
                ++ this.class.bank(window, channels)
        } {
            this.class.bank(allocation, n, 5) ++ this.class.bank(scale, n, 18)
        };
        ^this.class.bank(words, n) ++ other ++ [marker] ++ format
    }

    *width { |layer, channels| ^(if(layer == 3, 66, 96) * channels) + 8 }

    *fromWires { |layer, channels, wires|
        var n = 32 * channels, k = this.width(layer, channels) - 8;
        var words = wires.copyRange(0, n-1), first = wires.copyRange(n, (2*n)-1);
        var second, windows;
        if(layer == 3) {
            second = wires.copyRange(2*n, (2*n)+channels-1);
            windows = wires.copyRange((2*n)+channels, (2*n)+(2*channels)-1)
        } {
            second = wires.copyRange(2*n, (3*n)-1)
        };
        ^this.make(layer, channels, words, first, second, windows, wires[k], wires.copyRange(k+1,k+7))
    }

    done { ^state >= 2 }
    running { ^(state - 1).abs < 0.5 }
    codecRate { ^format[0] }
    bitrate { ^format[1] }
    frameStart { ^(marker - 1).abs < 0.5 }
}

MPEGDecodeOutput {
    var <audio, <state, <frames, <drops;
    *new { |wires, channels| ^super.new.init(wires, channels) }
    init { |wires, ch|
        audio = if(ch == 1, { wires[0] }, { wires.copyRange(0,ch-1) });
        state = wires[ch];
        frames = wires[ch+1];
        drops = wires[ch+2];
        ^this
    }
}

MPEGUnit : MultiOutUGen {
    *build { |count, args|
        var src = super.new.rate_(\audio).addToSynth;
        var signals = args.collect { |v|
            if(v.isNil or: { v.rate == \demand }) {
                Error("MPEG inputs require scalars, control signals or audio signals").throw
            };
            v.asUGenInput(src)
        };
        ^src.inputs_(signals).initOutputs(count, \audio).asArray
    }

    *header { |ch=1, capacity=1, mode=0, binputs=1, kbps=0, divisor=1, freeze=0,
        holdMode=0, normalized=0, run=1, record=1, clear=0, monitor=0, path|
        var chars, level = holdMode;
        if([1,2].includes(ch).not) { Error("Choose 1 or 2 channels in the SynthDef").throw };
        if(holdMode.isKindOf(Symbol)) {
            if([\coeff, \level].includes(holdMode).not) {
                Error("holdMode expects coeff, level or a signal").throw
            };
            level = if(holdMode == \level, 1, 0)
        };
        if(path.notNil and: { path.isString.not }) { Error("Supply the file path as a String").throw };
        chars = if(path.isNil, { [] }, { path.standardizePath.ascii.asArray.collect { |n| n.bitAnd(255) } });
        if(chars.size > 1023 or: { chars.includes(0) }) { Error("Invalid MPEG file path").throw };
        ^[ch, capacity, mode, binputs, kbps, divisor, freeze, level, normalized,
            run, record, clear, monitor, chars.size] ++ chars
    }

    *source { |ch, args, hasMonitor=false|
        var width = MPEGFrame.width(this.layer, ch), status = width + 8 + if(hasMonitor, ch, 0);
        var wires = this.build(status+3, args), frame = MPEGFrame.fromWires(this.layer, ch, wires);
        frame.bytes = wires.copyRange(width, width+7);
        if(hasMonitor) {
            frame.audio = if(ch == 1, { wires[width+8] }, { wires.copyRange(width+8, status-1) })
        };
        frame.state = wires[status];
        frame.frames = wires[status+1];
        frame.drops = wires[status+2];
        ^frame
    }
}

MPEGEncode : MPEGUnit {
    *ar { |in=0, bitrate=0, downsample=1|
        var inputs = in.asArray, ch = inputs.size;
        ^this.source(ch, this.header(ch, kbps:bitrate, divisor:downsample) ++ inputs)
    }
}

MPEGDecode : MPEGUnit {
    *ar { |fields, bitrate=0, freeze=0, holds=0, holdMode=\coeff, downsample=1, normalized=0|
        ^this.arInfo(fields, bitrate, freeze, holds, holdMode, downsample, normalized).audio
    }
    *arInfo { |fields, bitrate=0, freeze=0, holds=0, holdMode=\coeff, downsample=1, normalized=0|
        var ch = fields.channels;
        var args = this.header(ch, kbps:bitrate, divisor:downsample, freeze:freeze,
            holdMode:holdMode, normalized:normalized);
        args = args ++ fields.wires(this.layer) ++ MPEGFrame.bank(holds, 32*ch);
        ^MPEGDecodeOutput(this.build(ch+3, args), ch)
    }
    *bytes { |bytes, channels=2, freeze=0, holds=0, holdMode=\coeff, downsample=1|
        ^this.bytesInfo(bytes, channels, freeze, holds, holdMode, downsample).audio
    }
    *bytesInfo { |bytes, channels=2, freeze=0, holds=0, holdMode=\coeff, downsample=1|
        var lanes = bytes.asArray;
        var args = this.header(channels, mode:1, binputs:lanes.size, divisor:downsample,
            freeze:freeze, holdMode:holdMode);
        if(lanes.size < 1 or: { lanes.size > 64 }) { Error("Supply 1 to 64 packed byte lanes").throw };
        args = args ++ lanes ++ MPEGFrame.bank(holds, 32*channels);
        ^MPEGDecodeOutput(this.build(channels+3, args), channels)
    }
}

MPEGPack : MPEGUnit {
    *ar { |fields, path, run=1, bitrate=0, freeze=0, holds=0, holdMode=\coeff,
        downsample=1, normalized=0, monitor=0|
        var ch = fields.channels;
        var args = this.header(ch, kbps:bitrate, divisor:downsample, freeze:freeze, holdMode:holdMode,
            normalized:normalized, run:run, monitor:monitor, path:path);
        ^this.source(ch, args ++ fields.wires(this.layer) ++ MPEGFrame.bank(holds, ch*32), true)
    }
}

MPEGUnpack : MPEGUnit {
    *ar { |path, channels=2, run=1, downsample=1|
        if(path.isNil) { Error("Supply the server-side MPEG file path").throw };
        ^this.source(channels, this.header(channels, divisor:downsample, run:run, path:path))
    }
}

MPEGFrames : MPEGUnit {
    *ar { |fields, age=0, capacity=64, record=1, clear=0, downsample=1|
        var ch = fields.channels, args, wires;
        if(capacity.isInteger.not or: { capacity < 1 or: { capacity > 1024 } }) {
            Error("Frame history capacity must be 1 to 1024").throw
        };
        args = this.header(ch, capacity:capacity, divisor:downsample, record:record, clear:clear);
        wires = this.build(MPEGFrame.width(this.layer,ch),
            args ++ fields.wires(this.layer) ++ MPEGFrame.bank(age,32*ch));
        ^MPEGFrame.fromWires(this.layer, ch, wires)
    }
}

MPEGClock : MPEGUnit {
    *ar { |marker= -2, downsample=1|
        var signal = if(marker.isKindOf(MPEGFrame), { marker.marker }, { marker });
        ^this.build(4, this.header(divisor:downsample) ++ [signal])
    }
}

MP1Bands : MPEGUnit {
    *ar { |words=0, address= -1, allocation=7, scale=18, raw=0|
        var lanes = words.asArray.flat.size, addresses = address.asArray.flat;
        var dest = if(addresses.size > 1, { MPEGFrame.bank(addresses,32,-1) }, {
            Array.fill(32, { |i| (i + addresses.first.floor.clip(0,31)) % 32 })
        });
        ^this.build(3, this.header(mode:raw, binputs:if(lanes == 1,32,lanes)) ++ MPEGFrame.bank(words,32) ++ dest
            ++ MPEGFrame.bank(allocation,32,7) ++ MPEGFrame.bank(scale,32,18))
    }
}
MP1Serial : MPEGUnit {
    *ar { |word=0, address= -1, allocation=7, scale=18, raw=0|
        ^this.build(3, this.header(mode:raw) ++ [word,address,allocation,scale])
    }
}
MP3Short : MPEGUnit {
    *ar { |attenuation=0, window=2, row=0, trims=0, channels=1|
        var db = trims.asArray.flat;
        if(db.size == 3 and: { channels == 2 }) { db = db ++ db };
        ^this.build(channels*32, this.header(channels) ++ MPEGFrame.bank(attenuation, channels*32)
            ++ MPEGFrame.bank(window,channels) ++ [row] ++ MPEGFrame.bank(db,channels*3))
    }
}

MP1Encode : MPEGEncode { *layer { ^1 } }
MP1Decode : MPEGDecode {
    *layer { ^1 }
    *serial { |word=0, address= -1, allocation=7, scale=18, raw=0|
        ^MP1Serial.ar(word,address,allocation,scale,raw)
    }
}
MP1Pack : MPEGPack { *layer { ^1 } }
MP1Unpack : MPEGUnpack { *layer { ^1 } }
MP1Frames : MPEGFrames { *layer { ^1 } }
MP1Clock : MPEGClock { *layer { ^1 } }
MP2Encode : MPEGEncode { *layer { ^2 } }
MP2Decode : MPEGDecode { *layer { ^2 } }
MP2Pack : MPEGPack { *layer { ^2 } }
MP2Unpack : MPEGUnpack { *layer { ^2 } }
MP2Frames : MPEGFrames { *layer { ^2 } }
MP2Clock : MPEGClock { *layer { ^2 } }
MP3Encode : MPEGEncode { *layer { ^3 } }
MP3Decode : MPEGDecode { *layer { ^3 } }
MP3Pack : MPEGPack { *layer { ^3 } }
MP3Unpack : MPEGUnpack { *layer { ^3 } }
MP3Frames : MPEGFrames { *layer { ^3 } }
MP3Clock : MPEGClock { *layer { ^3 } }
