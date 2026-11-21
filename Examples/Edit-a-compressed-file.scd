// Boot the server with s.options.numWireBufs = 1024, then evaluate the block.
// Choose an MP1, MP2 or MP3 file and a separate destination with the same extension.
(
Dialog.openPanel({ |source|
    var layer = ["mp1","mp2","mp3"].indexOfEqual(PathName(source).extension.toLower);
    var readers = [MP1Unpack, MP2Unpack, MP3Unpack], writers = [MP1Pack, MP2Pack, MP3Pack];
    if(layer.isNil) { "Choose an .mp1, .mp2 or .mp3 file.".warn } {
        Dialog.savePanel({ |destination|
            if(source.standardizePath == destination.standardizePath) {
                "Choose a different file for the edited take.".warn
            } {
                ~mpegEdit = { |run=0, hold=0, gain=1, kbps=0|
                    var f = readers[layer].ar(source, channels:2, run:run);
                    var edited = f.withWords(f.words.collect { |word,i|
                        // Change bands 4 through 11 on both channels.
                        if((i % 32).inclusivelyBetween(4,11), { word * gain }, { word })
                    });
                    var gates = Array.fill(32, { |i| if(i == 3, { hold }, { 0 }) });
                    var p = writers[layer].ar(edited, destination, run:f.running,
                        bitrate:kbps, holds:gates, holdMode:\level);
                    A2K.kr(p.state).poll(Changed.kr(A2K.kr(p.state)), "writer state");
                    Silent.ar(2)
                }.play;
                "Ready. Start with ~mpegEdit.set(\\run, 1).".postln;
            }
        })
    }
})
)
// Evaluate separately while the source is playing.
~mpegEdit.set(\run, 1, \gain, 0.25);
~mpegEdit.set(\hold, 1);
~mpegEdit.set(\hold, 0, \kbps, 96);
~mpegEdit.set(\run, 0);
~mpegEdit.free;
