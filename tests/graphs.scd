// Write SynthDefs into an isolated test directory for the offline and live servers.
(
try {
    var dir = "MPEG_TEST_DIR".getenv;
    var save = { |name, graph| SynthDef(name, graph).writeDefFile(dir) };
    var enc = [MP1Encode, MP2Encode, MP3Encode];
    var dec = [MP1Decode, MP2Decode, MP3Decode];
    var pack = [MP1Pack, MP2Pack, MP3Pack], unpack = [MP1Unpack, MP2Unpack, MP3Unpack];
    var memory = [MP1Frames, MP2Frames, MP3Frames], clock = [MP1Clock, MP2Clock, MP3Clock];
    var root = thisProcess.nowExecutingPath.dirname.dirname;
    var help = (root +/+ "HelpSource/Classes/*.schelp").pathMatch
        ++ (root +/+ "HelpSource/Guides/*.schelp").pathMatch;
    if(help.size < 24) { Error("Missing MPEG help files").throw };
    help.do { |path|
        if(SCDoc.parseFileFull(path).isNil) { Error("Cannot parse help file " ++ path).throw }
    };
    (root +/+ "Examples/*.scd").pathMatch.do { |path|
        var text = File.readAllString(path), start = text.find("\n(\n"), end = text.find("\n)\n");
        if(text.copyRange(start+1,end+1).compile.isNil) { Error("Cannot compile " ++ path).throw }
    };
    3.do { |i|
        var prefix = "layer" ++ (i+1), ext = ".mp" ++ (i+1);
        var destination = dir +/+ (prefix ++ ext);
        var output = dir +/+ (prefix ++ "-edited" ++ ext);
        var raw = dir +/+ (prefix ++ "-osc" ++ ext);
        var kb = if(i == 0, 192, 128);
        save.value(prefix, { |record=1, bitrate=0, divisor=1, freeze=0|
            var input = SinOsc.ar([431,739] + (SinOsc.kr([3,5])*9)) * [0.12,0.08];
            var f = enc[i].ar(input, bitrate, divisor);
            var h = memory[i].ar(f, capacity:4);
            var p = pack[i].ar(h, destination, record, bitrate, freeze, monitor:1);
            var a = dec[i].arInfo(f), b = dec[i].bytesInfo(p.bytes), clk = clock[i].ar(f);
            Out.ar(0, a.audio ++ p.audio ++ b.audio ++
                [clk[1],p.frameStart,f.codecRate,f.bitrate,f.drops+p.drops+a.drops+b.drops,p.frames]);
        });
        save.value(prefix ++ "edit", { |run=1, change=1, divisor=1, replaceBitrate=0|
            var f = unpack[i].ar(destination, 2, run, divisor);
            var p = pack[i].ar(f.withWords(f.words * change), output, f.running, replaceBitrate, monitor:1);
            var original = dec[i].bytes(f.bytes, 2);
            Out.ar(0,p.audio ++ original ++ [f.state,p.state,p.frames,f.codecRate, f.bitrate, f.drops + p.drops]);
        });
        save.value(prefix ++ "osc", { |record=1, freeze=0, bandHold=0, word=4, age=0, erase=0, suspend=0|
            var words = [word,word.neg] ++ (0!30);
            var f = switch(i, 0,{MPEGFrame.mp1(words,7,18)},1,{MPEGFrame.mp2(words,7,18)},
                2,{MPEGFrame.mp3(words,0,190)});
            var m = memory[i].ar(f,[age]++(0!31),4,1-suspend,erase);
            var mask = [bandHold] ++ (0!31);
            var p = pack[i].ar(m,raw,record,kb,freeze,mask,\level,monitor:1);
            var d = dec[i].arInfo(m,0,freeze,mask,\level);
            Out.ar(0, [p.audio, d.audio, p.words[0], p.words[1], p.marker, p.state, m.words[0],m.words[1]]);
        });
        save.value(prefix ++ "liveEdit", { |run=0|
            var src=unpack[i].ar(dir +/+ (prefix ++ "-live" ++ ext),1,run);
            var copy=pack[i].ar(src,dir +/+ (prefix ++ " edited café" ++ ext),src.running);
            SendReply.kr(Impulse.kr(12), '/mpeg_edit', [A2K.kr(src.state), A2K.kr(copy.state),
                A2K.kr(copy.frames), A2K.kr(src.drops + copy.drops)]);
        });
        save.value(prefix ++ "live", { |record=0, bitrate=128|
            var f = enc[i].ar(SinOsc.ar(431)*0.1, bitrate);
            var p = pack[i].ar(f,dir +/+ (prefix ++ "-live" ++ ext),record,bitrate,monitor:1);
            var d = dec[i].bytesInfo(p.bytes,1);
            SendReply.kr(Impulse.kr(10), '/mpeg_test', [A2K.kr(p.state), A2K.kr(p.frames),
                A2K.kr(d.frames), A2K.kr(f.drops + p.drops + d.drops), Amplitude.kr(d.audio)]);
        });
    };
    save.value("bytehold", { |run=1,freeze=0,gate=0,divisor=1|
        var f=MP3Unpack.ar(dir +/+ "external.mp3",2,run);
        var d=MP3Decode.bytesInfo(f.bytes,2,freeze,[gate]++(0!63),downsample:divisor);
        Out.ar(0, d.audio ++ [f.state,d.frames,d.drops]);
    });
    save.value("fit", { |word=64, bitrate=128, scale=18, run=1|
        var data=MPEGFrame.mp3([word,word.neg]++(0!30),0,175);
        var invalid=SinOsc.ar(5777)*1e20;
        var p=MP3Pack.ar(data.withWords([invalid]++data.words.drop(1)),dir+/+"fitted.mp3",run,bitrate,monitor:1);
        var listen=MP3Decode.arInfo(data,bitrate);
        var preview=MP3Pack.ar(data,bitrate:bitrate,monitor:1);
        Out.ar(0,[p.audio,listen.audio,preview.audio,p.bitrate,p.words[0],p.state]);
    });
    save.value("special", {
        var serial = MP1Decode.serial(SinOsc.ar(37)*30);
        var bands = MP1Bands.ar(SinOsc.ar((1..32)*13)*30);
        var clk = MP3Clock.ar, atten = MP3Short.ar(10,2,clk[0],[0,-6,-12]);
        Out.ar(0, serial ++ bands ++ [atten[3],clk[0],clk[1],clk[2]]);
    });
    "MPEG SynthDefs compiled".postln;
    0.exit;
} { |err| err.reportError; 1.exit; };
)
