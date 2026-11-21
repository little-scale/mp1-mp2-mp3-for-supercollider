#!/usr/bin/env python3
"""Exercise the installed language and servers, and compare their audio with FFmpeg."""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get("SC_APP", "/Applications/SuperCollider.app")) / "Contents"
RESOURCES = APP / "Resources"


def padded(data):
    return data + bytes((-len(data)) % 4)


def string(value):
    return padded(str(value).encode() + b"\0")


def osc(address, *values):
    types = ","
    args = []
    for value in values:
        if isinstance(value, str):
            types += "s"
            args.append(string(value))
        elif isinstance(value, int):
            types += "i"
            args.append(struct.pack(">i", value))
        else:
            types += "f"
            args.append(struct.pack(">f", value))
    return string(address) + string(types) + b"".join(args)


def size_prefix(payload):
    return struct.pack(">i", len(payload)) + payload


def bundle(seconds, messages):
    timestamp = int(seconds * 2**32)
    return (
        b"#bundle\0"
        + struct.pack(">Q", timestamp)
        + b"".join(map(size_prefix, messages))
    )


def command(args, **kwargs):
    proc = subprocess.run(
        list(map(str, args)),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=65,
        check=False,
        **kwargs,
    )
    log = proc.stdout.decode(errors="replace")
    if proc.returncode or any(
        msg in log
        for msg in ["FAILURE IN SERVER", "invalid UGen layout", "MPEG Audio:", "ERROR:"]
    ):
        raise RuntimeError(f"{args[0]} failed ({proc.returncode})\n{log[-4000:]}")
    return log


def compile_graphs(work):
    config = work / "language.yaml"
    config.write_text(
        "includePaths:\n  - "
        + json.dumps(str(ROOT / "Classes"))
        + "\nexcludePaths: []\npostInlineWarnings: false\n"
    )
    log = command(
        [APP / "MacOS/sclang", "-D", "-l", config, ROOT / "tests/graphs.scd"],
        env={**os.environ, "MPEG_TEST_DIR": str(work)},
    )
    assert "MPEG SynthDefs compiled" in log


def server_options():
    return [
        "-D",
        "0",
        "-R",
        "0",
        "-U",
        f"{RESOURCES / 'plugins'}:{ROOT / 'plugins'}",
        "-w",
        "1536",
        "-m",
        "65536",
        "-i",
        "0",
    ]


def render(work, name, channels, rate, length, initial=(), changes=(), arch=None):
    defpath = work / (name + ".scsyndef")
    events = [
        (0, [osc("/d_load", str(defpath)), osc("/s_new", name, 1000, 0, 0, *initial)])
    ]
    events.extend((t, [osc("/n_set", 1000, *settings)]) for t, settings in changes)
    events.append((length, [osc("/n_free", 1000)]))
    score, wav = work / (name + ".osc"), work / (name + ".wav")
    score.write_bytes(b"".join(size_prefix(bundle(t, msgs)) for t, msgs in events))
    argv = [
        RESOURCES / "scsynth",
        *server_options(),
        "-s",
        "0",
        "-o",
        channels,
        "-N",
        score,
        "_",
        wav,
        rate,
        "WAV",
        "float",
    ]
    if arch:
        argv = ["arch", "-" + arch, *argv]
    command(argv)
    return samples(wav, channels)


def samples(path, channels):
    raw = subprocess.check_output(
        [
            "ffmpeg",
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:a:0",
            "-f",
            "f32le",
            "-c:a",
            "pcm_f32le",
            "-",
        ]
    )
    result = np.frombuffer(raw, "<f4").reshape(-1, channels)
    assert result.size and np.isfinite(result).all(), path
    return result


def starts(wire):
    return np.flatnonzero(wire > 0.5)


def aligned(reference, signal, rate):
    # Use a non-stationary section to locate the fixed pipeline delay, then compare the full overlap.
    lo, count = rate // 8, rate // 4
    anchor = reference[lo : lo + count, 0].astype(float)
    search = signal[: min(len(signal), lo + count + rate // 2), 0].astype(float)
    n = 1 << (len(search) + len(anchor) - 1).bit_length()
    cross = np.fft.irfft(np.fft.rfft(search, n) * np.fft.rfft(anchor[::-1], n), n)
    energy = np.r_[0.0, np.cumsum(search * search)]
    norm = np.sqrt(np.maximum(1e-30, energy[count:] - energy[:-count]))
    lag = int(np.argmax(cross[count - 1 : len(search)] / norm) - lo)
    r0, s0 = max(0, -lag), max(0, lag)
    length = min(len(reference) - r0, len(signal) - s0)
    return reference[r0 : r0 + length], signal[s0 : s0 + length], lag


def snr(reference, signal):
    return float(
        10
        * np.log10(
            np.mean(reference.astype(float) ** 2)
            / max(1e-30, np.mean((reference.astype(float) - signal) ** 2))
        )
    )


def file_info(path):
    data = subprocess.check_output(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_streams",
            "-of",
            "json",
            str(path),
        ]
    )
    return json.loads(data)["streams"][0]


def native(work, layer, arch=None):
    name = "layer" + str(layer)
    first, second = (192, 224) if layer == 1 else (128, 192)
    a = render(
        work,
        name,
        12,
        48000,
        1.4,
        ["bitrate", first],
        [(0.56, ["bitrate", second]), (1.25, ["record", 0])],
        arch,
    )
    file = work / (name + ".mp" + str(layer))
    info = file_info(file)
    assert (info["codec_name"], int(info["sample_rate"]), info["channels"]) == (
        "mp" + str(layer),
        48000,
        2,
    )
    reference = samples(file, 2)
    assert {first, second}.issubset(set(a[:, 9])) and a[:, 10].max() == 0
    assert (
        np.max(np.abs(reference[:, 0])) > 0.06
        and np.max(np.abs(reference[:, 1])) > 0.04
    )
    for offset in [0, 2, 4]:
        r, s, lag = aligned(reference, a[:, offset : offset + 2], 48000)
        # The field path can lead the packed output by a frame at a live bitrate change.
        check = slice(4800, min(18000, len(r)))
        measured = snr(r[check], s[check])
        print(
            f"MP{layer} {['fields', 'packed monitor', 'bytes'][offset // 2]} vs FFmpeg: "
            f"{measured:.1f} dB SNR, alignment {lag} samples",
            flush=True,
        )
        assert measured > 48, (layer, offset, measured)
    for k in [6, 7]:
        edges = starts(a[:, k])
        assert len(edges) > 35 and np.all(
            np.diff(edges) == (384 if layer == 1 else 1152)
        ), (layer, k, np.unique(np.diff(edges)))
    return file


def header_rates(path):
    data = path.read_bytes()
    i = 0
    found = []
    tables = [
        [
            [32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
            [8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
            [8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
        ],
        [
            [32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
            [32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
            [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
        ],
    ]
    while i < len(data):
        h = data[i : i + 4]
        assert len(h) == 4 and h[0] == 255 and h[1] & 224 == 224, (path, i)
        version = (h[1] >> 3) & 3
        layer = 4 - ((h[1] >> 1) & 3)
        hz = [44100, 48000, 32000][(h[2] >> 2) & 3] // {3: 1, 2: 2, 0: 4}[version]
        kb = tables[version == 3][layer - 1][(h[2] >> 4) - 1]
        pad = (h[2] >> 1) & 1
        if layer == 1:
            size = 4 * (12000 * kb // hz + pad)
        else:
            size = (72000 if layer == 3 and version != 3 else 144000) * kb // hz + pad
        i += size
        found.append((kb, hz))
    assert i == len(data)
    return found


def edit_and_hold(work, l, arch):
    name = f"layer{l}"
    source = work / f"{name}.mp{l}"
    original = samples(source, 2)
    destination = work / f"{name}-edited.mp{l}"
    rates = header_rates(source)
    assert {rate for rate, hz in rates} == ({192, 224} if l == 1 else {128, 192})
    for sign in [1, -1]:
        a = render(work, name + "edit", 10, 48000, 1.6, ["change", sign], arch=arch)
        result = samples(destination, 2)
        assert a[-150, 4] == 2 and a[-150, 5] == 2 and a[:, 9].max() == 0
        assert len(result) == len(original) and header_rates(destination) == rates
        if sign == 1:
            assert np.array_equal(original, result), l
        else:
            assert snr(original, -result) > 65, l
    for hz, div in [(96000, 2), (44100, 1), (50000, 2)]:
        a = render(work, name + "edit", 10, hz, 1.65, ["divisor", div], arch=arch)
        result = samples(destination, 2)
        headers = header_rates(destination)
        assert {r for k, r in headers} == {48000 // div}
        assert abs(len(result) * div - len(original)) < (2304 if l != 1 else 768)
        assert a[-180, 4:6].tolist() == [2, 2] and a[:, 9].max() == 0
        assert np.std(result, axis=0).min() > 0.035
        # Resampling to the server rate preserves pitch and time.
        center = a[hz // 4 : 3 * hz // 4, 0]
        peak = (
            np.argmax(abs(np.fft.rfft(center * np.hanning(len(center)))))
            * hz
            / len(center)
        )
        assert 415 < peak < 447, (l, hz, peak)
    changes = [
        (0.20, ["bandHold", 1]),
        (0.30, ["word", 8]),
        (0.45, ["freeze", 1]),
        (0.53, ["word", 2]),
        (0.65, ["freeze", 0, "bandHold", 0]),
        (0.76, ["suspend", 1, "age", 2]),
        (0.82, ["word", 9]),
        (0.98, ["erase", 1]),
        (1.08, ["record", 0]),
    ]
    a = render(work, name + "osc", 8, 48000, 1.2, ["word", 4], changes, arch)
    for t, expected in [
        (0.18, [4, -4]),
        (0.40, [4, -8]),
        (0.61, [4, -8]),
        (0.745, [2, -2]),
        (0.94, [2, -9]),
        (1.07, [9, -9]),
    ]:
        assert a[int(t * 48000), 2:4].tolist() == expected, (l, t, a[int(t * 48000)])
    assert np.std(a[:, 0]) > 0.001 and np.std(a[:, 1]) > 0.001
    assert np.std(samples(work / f"{name}-osc.mp{l}", 1)) > 0.001
    print(
        f"MP{l} file round trip exact; inversion, band holds, history and three host clocks passed",
        flush=True,
    )


def low_rate(work, l, arch):
    name = f"layer{l}"
    kb = 32 if l == 1 else 8
    div = 6 if l == 3 else 2
    native_rate = 8000 if l == 3 else 24000
    a = render(
        work,
        name,
        12,
        96000,
        1.5,
        ["bitrate", kb, "divisor", div],
        [(1.35, ["record", 0])],
        arch,
    )
    headers = header_rates(work / f"{name}.mp{l}")
    assert set(headers) == {(kb, native_rate)} and a[:, 10].max() == 0, (
        l,
        set(headers),
    )
    assert np.std(a[48000:96000, :6], axis=0).min() > 0.009
    # Sent controls also change clock during one synth's lifetime. A take closes
    # on a format change; the new rate remains available on the live patch.
    a = render(
        work, name, 12, 48000, 1.4, ["bitrate", 96], [(0.55, ["divisor", 2])], arch
    )
    assert {48000, 24000}.issubset(set(a[:, 8])) and a[:, 10].max() == 0
    assert {hz for kb, hz in header_rates(work / f"{name}.mp{l}")} == {48000}
    print(
        f"MP{l}: {kb} kb/s at {native_rate} Hz, 96 kHz server and live clock change passed",
        flush=True,
    )


def joint_stereo(work, arch):
    path = work / "external.mp3"
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            (
                "aevalsrc=0.12*sin(2*PI*431*t+2*sin(23*t))+0.035*sin(2*PI*739*t)|"
                "0.11*sin(2*PI*431*t+2*sin(23*t))+0.028*sin(2*PI*739*t+0.7*sin(27*t)):s=44100:d=1.3"
            ),
            "-c:a",
            "libmp3lame",
            "-q:a",
            "3",
            "-joint_stereo",
            "1",
            "-write_xing",
            "0",
            "-id3v2_version",
            "0",
            str(path),
        ],
        check=True,
    )
    raw = path.read_bytes()
    pos = 0
    mid_side = reservoir = 0
    rates = header_rates(path)
    for kb, hz in rates:
        h = raw[pos : pos + 6]
        mid_side += (h[3] >> 6) == 1 and bool(h[3] & 0x20)
        reservoir += bool((h[4] << 1) | (h[5] >> 7))
        pos += 144000 * kb // hz + ((h[2] >> 1) & 1)
    assert mid_side > 1 and reservoir > 1 and len(set(rates)) > 1
    reference = samples(path, 2)
    a = render(work, "bytehold", 5, 44100, 1.7, arch=arch)
    r, s, lag = aligned(reference, a[:, :2], 44100)
    clean = slice(2500, len(r) - 1500)
    score = snr(r[clean], s[clean])
    assert score > 85 and a[:, 4].max() == 0, (score, lag)
    b = render(
        work,
        "bytehold",
        5,
        44100,
        1.7,
        [],
        [
            (0.3, ["gate", 1]),
            (0.48, ["freeze", 1]),
            (0.69, ["freeze", 0]),
            (0.91, ["gate", 0]),
        ],
        arch,
    )
    assert np.mean(np.abs(b[26000:28000, :2] - a[26000:28000, :2])) > 0.003
    assert np.max(np.abs(b[47500:53500, :2] - a[47500:53500, :2])) < 0.0001
    c = render(work, "bytehold", 5, 48000, 1.7, ["divisor", 2], arch=arch)
    assert c[:, 4].max() == 0 and c[18000:44000, 0].std() > 0.05
    print(
        f"Independent VBR joint-stereo MP3: {score:.1f} dB vs FFmpeg; reservoir, hold/release and divided byte playback passed",
        flush=True,
    )


def external_files(work, arch):
    source = work / "layer2.mp2"
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "aevalsrc=0.1*sin(2711*t)|0.08*sin(4531*t):s=44100:d=1.2",
            "-c:a",
            "mp2",
            "-b:a",
            "160k",
            "-f",
            "mp2",
            str(source),
        ],
        check=True,
    )
    render(work, "layer2edit", 10, 48000, 1.55, arch=arch)
    assert np.array_equal(samples(source, 2), samples(work / "layer2-edited.mp2", 2))
    source = work / "layer3.mp3"
    shutil.copyfile(work / "external.mp3", source)
    render(work, "layer3edit", 10, 48000, 1.6, ["replaceBitrate", 320], arch=arch)
    a, b = samples(source, 2), samples(work / "layer3-edited.mp3", 2)
    assert a.shape == b.shape
    score = snr(a, b)
    assert score > 40, score
    print(
        f"Foreign files: FFmpeg MP2 exact; edited L/R fields from joint-stereo MP3 repacked at 320 kb/s, {score:.1f} dB SNR",
        flush=True,
    )


def special(work, arch):
    a = render(work, "special", 10, 48000, 0.4, arch=arch)
    for k in [0, 3]:
        assert a[:, k].std() > 0.01
    assert a[:, 1].max() == 31 and a[:, 4].max() == 31
    assert set(a[64:-64, 6]) == {10, 14, 18} and set(a[64:-64, 7]) == set(range(18))
    assert np.all(np.diff(starts(a[:, 8])) == 1152) and np.all(
        np.diff(starts(a[:, 9])) == 576
    )
    a = render(
        work,
        "fit",
        6,
        48000,
        0.9,
        ["bitrate", 32],
        [(0.38, ["bitrate", 320]), (0.8, ["run", 0])],
        arch,
    )
    assert {k for k, r in header_rates(work / "fitted.mp3")} == {32, 320}
    assert np.max(np.abs(a[:, 4])) == 8206 and np.max(np.abs(a[:, 1] - a[:, 2])) < 1e-5
    assert np.isfinite(samples(work / "fitted.mp3", 1)).all()
    print(
        "Serial and parallel MP1, MP3 short windows, clamping and decoder bitrate preview passed",
        flush=True,
    )


def unpack_message(packet):
    def read_string(offset):
        end = packet.index(b"\0", offset)
        return packet[offset:end].decode(), (end + 4) & ~3

    address, offset = read_string(0)
    types, offset = read_string(offset)
    values = []
    for code in types[1:]:
        if code == "s":
            item, offset = read_string(offset)
        else:
            (item,) = struct.unpack_from(">" + code, packet, offset)
            offset += struct.calcsize(">" + code)
        values.append(item)
    return address, values


def live(work, engine, arch=None, restrict_files=False):
    # Use a dedicated server and silent graphs. Never connect to the user's running server.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    argv = [
        str(RESOURCES / engine),
        *server_options(),
        "-u",
        str(port),
        "-o",
        "2",
        "-Z",
        "256",
    ]
    if restrict_files:
        argv.extend(["-P", str(work)])
    if engine == "supernova":
        argv.extend(["-T", "2"])
    if arch:
        argv = ["arch", "-" + arch, *argv]
    log_path = work / (engine + "-live.log")
    with (
        log_path.open("wb") as log,
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client,
    ):
        client.bind(("127.0.0.1", 0))
        client.settimeout(0.15)
        process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
        replies = {1001: [], 1002: [], 1003: []}
        copies = {}

        def send(address, *args):
            client.sendto(osc(address, *args), ("127.0.0.1", port))

        def receive():
            if process.poll() is not None:
                raise RuntimeError(
                    f"{engine} exited with {process.returncode}\n{log_path.read_text()}"
                )
            try:
                addr, values = unpack_message(client.recv(8192))
            except TimeoutError:
                return "", []
            if addr == "/fail":
                raise RuntimeError(str(values) + "\n" + log_path.read_text())
            if addr == "/mpeg_test":
                replies[values[0]].append(values[2:])
            if addr == "/mpeg_edit":
                copies[values[0]] = values[2:]
            return addr, values

        def wait_for(addr, timeout=6):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if receive()[0] == addr:
                    return
            raise RuntimeError(
                "Timeout waiting for " + addr + "\n" + log_path.read_text()
            )

        def collect(duration):
            end = time.monotonic() + duration
            while time.monotonic() < end:
                receive()

        try:
            for _ in range(45):
                send("/status")
                if receive()[0] == "/status.reply":
                    break
            else:
                raise RuntimeError("Server did not boot\n" + log_path.read_text())
            send("/notify", 1)
            wait_for("/done")
            for l in [1, 2, 3]:
                send("/d_load", str(work / f"layer{l}live.scsyndef"))
                wait_for("/done")
                send("/s_new", f"layer{l}live", 1000 + l, 0, 0)
            collect(0.9)
            for id in replies:
                send("/n_set", id, "record", 1, "bitrate", 160)
            collect(0.55)
            for id in replies:
                send("/n_set", id, "bitrate", 96)
            collect(0.55)
            for id in replies:
                send("/n_set", id, "record", 0)
            collect(0.55)
            for id, values in replies.items():
                assert values, (engine, id, "missing status")
                stats = np.array(values)
                assert stats[-1, 0] == 2 and stats[-1, 1] > 15 and stats[-1, 2] > 25, (
                    engine,
                    id,
                    stats[-3:],
                )
                assert stats[:, 3].max() == 0 and stats[:, 4].max() > 0.03, (
                    engine,
                    id,
                    stats[-3:],
                )
                path = work / f"layer{id - 1000}-live.mp{id - 1000}"
                assert samples(path, 1).std() > 0.04, path
                print(
                    f"{engine} MP{id - 1000}: {stats[-1, 1]:.0f} frames written, "
                    f"{stats[-1, 2]:.0f} decoded, zero queue drops",
                    flush=True,
                )
            for id in replies:
                send("/n_free", id)
            for l in [1, 2, 3]:
                send("/d_load", str(work / f"layer{l}liveEdit.scsyndef"))
                wait_for("/done")
                send("/s_new", f"layer{l}liveEdit", 1100 + l, 0, 0, "run", 1)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                receive()
                if len(copies) == 3 and all(v[1] == 2 for v in copies.values()):
                    break
            for l in [1, 2, 3]:
                result = copies.get(1100 + l)
                assert result and result[:2] == [2, 2] and result[3] == 0, (
                    engine,
                    l,
                    result,
                )
                original = samples(work / f"layer{l}-live.mp{l}", 1)
                edited = samples(work / f"layer{l} edited café.mp{l}", 1)
                assert np.array_equal(original, edited), (
                    engine,
                    l,
                    len(original),
                    len(edited),
                )
                send("/n_free", 1100 + l)
            print(
                f"{engine}: all three live file edits finalized at EOF and matched sample-for-sample",
                flush=True,
            )
            # Free before asynchronous construction finishes, then check server liveness.
            for i in range(12):
                client.sendto(
                    bundle(
                        2**-32,
                        [
                            osc("/s_new", "layer3live", 2000 + i, 0, 0),
                            osc("/n_free", 2000 + i),
                        ],
                    ),
                    ("127.0.0.1", port),
                )
            collect(0.3)
            send("/sync", 20)
            wait_for("/synced")
            send("/quit")
            process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        log.flush()
        content = log_path.read_text()
        assert (
            process.returncode == 0
            and "MPEG Audio:" not in content
            and "FAILURE IN SERVER" not in content
        ), content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path)
    parser.add_argument(
        "--compiled", action="store_true", help="reuse SynthDefs in --work"
    )
    parser.add_argument("--arch", choices=["arm64", "x86_64"])
    parser.add_argument(
        "--live", action="store_true", help="also boot both local audio servers"
    )
    parser.add_argument("--live-only", action="store_true")
    parser.add_argument(
        "--restrict-files",
        action="store_true",
        help="exercise live file objects with the server restricted to the test directory",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as t:
        work = (args.work or Path(t)).resolve()
        work.mkdir(exist_ok=True, parents=True)
        if not args.compiled:
            compile_graphs(work)
        if not args.live_only:
            for l in [1, 2, 3]:
                native(work, l, args.arch)
                edit_and_hold(work, l, args.arch)
                low_rate(work, l, args.arch)
            special(work, args.arch)
            joint_stereo(work, args.arch)
            external_files(work, args.arch)
        if args.live or args.live_only:
            for server in ["scsynth", "supernova"]:
                live(work, server, args.arch, args.restrict_files)
        print("SuperCollider MPEG checks passed", flush=True)


if __name__ == "__main__":
    main()
