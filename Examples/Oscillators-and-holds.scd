// First set s.options.numWireBufs = 1024 and boot the server.
// Evaluate this block, then evaluate the individual control lines below.
(
~mpegVoice = { |speed=1, bandHold=0, allHold=0, kbps=0|
    var phase = SinOsc.ar([1.7, 2.9, 4.1, 7.3] * speed);
    var fields = MPEGFrame.mp3(phase * [14,10,7,3], gain:171);
    var mask = [0, bandHold] ++ (0 ! 30);
    var audio = MP3Decode.ar(fields, bitrate:kbps, freeze:allHold, holds:mask, holdMode:\level);
    Limiter.ar(LeakDC.ar(audio), 0.2) ! 2
}.play;
)
~mpegVoice.set(\bandHold, 1);
~mpegVoice.set(\speed, 3, \kbps, 64);
~mpegVoice.set(\allHold, 1);
~mpegVoice.set(\allHold, 0, \bandHold, 0);
~mpegVoice.free;
