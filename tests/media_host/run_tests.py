"""Compile the actual C modules with tiny IDF stubs and run bounded-I/O regressions.
Usage: python tests/media_host/run_tests.py --cc /path/to/gcc
Runs entirely in a temporary directory under build; never accesses a real SD card.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

class Audio(C.Structure):
    _fields_ = [("file_size", C.c_uint64), ("total_samples", C.c_uint64),
                ("duration_ms", C.c_uint64), ("sample_rate", C.c_uint32),
                ("bits", C.c_uint8), ("channels", C.c_uint8),
                ("is_flac", C.c_bool), ("metadata_known", C.c_bool)]
class Item(C.Structure):
    _fields_ = [(n, C.c_char * 256) for n in ("name", "audio", "cover", "lyric")]
class Page(C.Structure):
    _fields_ = [("first", C.c_uint32), ("count", C.c_uint32),
                ("names", (C.c_char * 256) * 8)]

def flac_header(rate=192000, bits=32, channels=2, samples=192000*600):
    data = bytearray(42)
    data[:8] = b"fLaC\x80\x00\x00\x22"
    data[8:12] = bytes.fromhex("10001000")
    packed = (rate << 44) | ((channels-1) << 41) | ((bits-1) << 36) | samples
    data[18:26] = packed.to_bytes(8, "big")
    return data

def tests(lib):
    lib.lyric_parser_load.argtypes = [C.c_char_p]
    lib.lyric_parser_get_current_text.argtypes = [C.c_uint32]
    lib.lyric_parser_get_current_text.restype = C.c_char_p
    lib.lyric_parser_get_page.argtypes = [C.c_uint32, C.c_void_p, C.c_size_t]
    lib.lyric_parser_is_truncated.restype = C.c_bool
    lib.test_live_bytes.restype = C.c_size_t
    lib.test_peak_bytes.restype = C.c_size_t
    lib.test_set_inserted.argtypes = [C.c_bool]
    lib.audio_player_probe.argtypes = [C.c_char_p, C.POINTER(Audio)]
    lib.music_library_get_item.argtypes = [C.c_uint32, C.POINTER(Item)]
    lib.music_library_get_page.argtypes = [C.c_uint32, C.POINTER(Page)]
    lib.music_library_is_truncated.restype = C.c_bool
    def lrc(data):
        Path("test.lrc").write_bytes(data.encode("utf-8") if isinstance(data, str) else data)
        return lib.lyric_parser_load(b"test.lrc")

    assert lrc("\ufeff[ti:Long metadata must not become a lyric]\r\n[00:03.123]third\r\n"
               "[00:01.2][00:02.34]shared\r\n[offset:100]\r\n") == 0
    assert lib.lyric_parser_get_line_count() == 3
    assert lib.lyric_parser_get_current_text(1099) == b""
    assert lib.lyric_parser_get_current_text(1100) == b"shared"
    assert lib.lyric_parser_get_current_text(2240) == b"shared"
    assert lib.lyric_parser_get_current_text(3023) == b"third"
    buf = C.create_string_buffer(4096)
    lib.lyric_parser_get_page(0, buf, len(buf))
    assert buf.value == b"shared\n\nshared\n\nthird"
    print("PASS timestamps, BOM, CRLF, metadata, offset, multiple tags, sorting")

    assert lrc("[" + "a"*900 + "]bad\n[00:00.000]\u4f60\u597d\n") == 0
    assert lib.lyric_parser_get_current_text(0).decode() == "\u4f60\u597d"
    assert lrc(b"\xff\xfe[\x000\x00") == 0x106
    assert lib.test_live_bytes() == 0
    assert lrc(b"[00:01]\xe4\xb8\n") == 0x106
    assert lrc(b"[00:01]\xed\xa0\x80\n") == 0x106
    assert lrc(b"a"*(256*1024+1)) == 0x104
    assert lib.test_live_bytes() == 0
    assert lrc("[00:99.12]bad\n[00:01.1234]bad\n[00:02]ok\n") == 0
    assert lib.lyric_parser_get_line_count() == 1
    assert lrc("[00:00]" + "x"*2000 + "\n[00:01]ok\n") == 0
    assert lib.lyric_parser_is_truncated()
    assert lib.lyric_parser_get_line_count() == 1
    print("PASS malformed/long tags, invalid UTF-8, UTF-16 rejection, input bounds")

    assert lrc("".join(f"[00:01]{i}\n" for i in range(1100))) == 0
    assert lib.lyric_parser_get_line_count() == 1024
    assert lib.lyric_parser_is_truncated()
    assert lrc("".join("[00:01]" + "x"*500 + "\n" for _ in range(150))) == 0
    assert lib.lyric_parser_get_line_count() < 150
    assert lib.lyric_parser_is_truncated()
    for fail in (0, 1):
        lib.test_fail_after(fail)
        assert lrc("[00:01]allocation failure\n") == 0x101
        assert lib.test_live_bytes() == 0
    lib.test_fail_after(-1)
    rng = random.Random(20261017)
    for _ in range(500):
        raw = bytes(rng.randrange(256) for _ in range(rng.randrange(2000)))
        lrc(raw)
        lib.lyric_parser_destroy()
        assert lib.test_live_bytes() == 0
    for _ in range(100):
        assert lrc("[00:00.001]repeat\n") == 0
        lib.lyric_parser_destroy()
        assert lib.test_live_bytes() == 0
    assert lib.test_peak_bytes() <= 96*1024
    print("PASS bounded arena/line count, allocation failures, 500 fuzz inputs, repeated load/free")

    samples = (1 << 35) + 12345  # exceeds 32-bit sample counts
    Path("large.flac").write_bytes(flac_header(samples=samples))
    with open("large.flac", "r+b") as f:
        if os.name == "nt":
            import msvcrt
            returned = C.c_ulong()
            ok = C.windll.kernel32.DeviceIoControl(C.c_void_p(msvcrt.get_osfhandle(f.fileno())),
                   0x900C4, None, 0, None, 0, C.byref(returned), None)
            assert ok, "Cannot create sparse test file"
        f.truncate(300*1024*1024)
    info = Audio()
    assert lib.audio_player_probe(b"large.flac", C.byref(info)) == 0
    assert (info.sample_rate, info.bits, info.channels) == (192000, 32, 2)
    assert info.total_samples == samples and info.duration_ms == samples*1000//192000
    assert info.file_size == 300*1024*1024 and info.metadata_known
    Path("bad.flac").write_bytes(b"not flac")
    assert lib.audio_player_probe(b"bad.flac", C.byref(info)) != 0
    Path("sample.MP3").write_bytes(b"ID3" + b"\0"*40)
    assert lib.audio_player_probe(b"sample.MP3", C.byref(info)) == 0
    assert not info.metadata_known and info.duration_ms == 0
    assert lib.test_live_bytes() == 0
    print("PASS 300-MiB sparse FLAC, 32-bit/192-kHz metadata, 36-bit sample count, MP3 unknown duration")

    # Real cover_loader.c with a fake JPEG engine: no claim of hardware decode validation.
    lib.cover_loader_load.argtypes = [C.c_char_p, C.POINTER(C.c_void_p)]
    lib.cover_loader_commit.argtypes = [C.c_void_p]
    ptr = C.c_void_p()
    def cover(w, h, value):
        Path("cover.jpg").write_bytes(b"JPGT"+w.to_bytes(2,"big")+h.to_bytes(2,"big")+bytes([value]))
        return lib.cover_loader_load(b"cover.jpg", C.byref(ptr))
    assert cover(240, 240, 11) == 0
    first = ptr.value
    lib.cover_loader_commit(ptr)
    assert cover(240, 240, 22) == 0 and ptr.value != first
    second = ptr.value
    assert C.string_at(first, 1) == b"\x0b"
    # Cancel this result: without commit the same inactive slot is reused.
    assert cover(240, 240, 33) == 0 and ptr.value == second
    assert C.string_at(first, 1) == b"\x0b"
    lib.cover_loader_commit(ptr)
    assert cover(240, 240, 44) == 0 and ptr.value == first
    assert C.string_at(second, 1) == b"!"
    assert cover(241, 240, 55) == 0x104 and not ptr.value
    assert cover(240, 239, 55) == 0x104 and not ptr.value
    Path("cover.jpg").write_bytes(b"x"*(256*1024+1))
    assert lib.cover_loader_load(b"cover.jpg", C.byref(ptr)) == 0x104
    lib.cover_loader_commit(None)
    lib.cover_loader_destroy()
    assert lib.test_live_bytes() == 0
    for fail in (0, 1, 2):
        lib.test_fail_after(fail)
        assert cover(240, 240, 1) == 0x101
        lib.cover_loader_destroy()
        assert lib.test_live_bytes() == 0
    lib.test_fail_after(-1)
    print("PASS cover mock: 240x240/size limits, double-buffer ownership, cancelled loads, allocation failure cleanup")

    sysdir = Path("test_sd/HEI_MAO_POD_SYS")
    music = sysdir / "MUSIC"
    music.mkdir(parents=True)
    (sysdir / ".hmcache").mkdir()
    for i in range(1300):
        d = music / f"song{i:04d}"
        d.mkdir()
        (d / "Track.FLAC").write_bytes(flac_header())
        (d / "track.JPG").write_bytes(b"cover")
        (d / "TRACK.LRC").write_bytes(b"[00:01]lyrics")
    (music / "ignored.mp3").write_bytes(b"root file ignored")
    (music / "empty").mkdir()
    missing = music / "missing_sidecar"
    missing.mkdir()
    (missing / "x.mp3").write_bytes(b"audio")
    (missing / "wrong.jpg").write_bytes(b"must not match")
    assert lib.music_library_scan() == 0
    assert lib.music_library_get_count() == 1301
    assert not lib.music_library_is_truncated()
    assert (sysdir / ".hmcache/music.idx").stat().st_size == 1301*C.sizeof(Item)
    item, page = Item(), Page()
    names = set()
    for start in range(0, 1301, 8):
        assert lib.music_library_get_page(start, C.byref(page)) == 0
        assert 1 <= page.count <= 8
        for i in range(page.count): names.add(bytes(page.names[i]).split(b"\0")[0])
    assert len(names) == 1301
    for i in range(1301):
        assert lib.music_library_get_item(i, C.byref(item)) == 0
        if item.name == b"missing_sidecar":
            assert item.cover == b"" and item.lyric == b""
        else:
            assert item.audio == b"Track.FLAC" and item.cover == b"track.JPG" and item.lyric == b"TRACK.LRC"
    assert lib.music_library_get_item(1301, C.byref(item)) != 0
    assert lib.music_library_scan() == 0 and lib.music_library_get_count() == 1301
    print("PASS 1301-song disk index, 8-row transfer pages, uppercase extensions, same-stem sidecars, rescan")

    with open(sysdir / ".hmcache/music.idx", "r+b") as f:
        f.write(b"x"*256)  # missing NUL in untrusted cache
    assert lib.music_library_get_item(0, C.byref(item)) != 0
    lib.test_set_inserted(False)
    assert lib.music_library_scan() != 0 and lib.music_library_get_count() == 0
    lib.test_set_inserted(True)
    lib.music_library_reset()
    assert lib.test_live_bytes() == 0
    print("PASS corrupted index rejection, absent-card cancellation, empty reset")
    print("ALL MEDIA HOST TESTS PASSED")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("gcc"))
    args = parser.parse_args()
    if not args.cc: parser.error("Specify --cc with a native GCC compiler")
    build = REPO / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="media-test-", dir=build) as temporary:
        temp = Path(temporary).resolve()
        assert temp.is_relative_to(build.resolve())  # constrain recursive temporary cleanup
        dll = temp / ("media.dll" if os.name == "nt" else "media.so")
        command = [args.cc, "-std=c11", "-shared", "-O1", "-g", "-Wall", "-Wextra",
                   "-Wno-unused-variable", "-Wno-sign-compare", "-static-libgcc",
                   '-DSD_MOUNT_POINT="test_sd"', "-I"+str(HERE/"stubs"), "-I"+str(REPO/"main"),
                   str(REPO/"main/lyric_parser.c"), str(REPO/"main/audio_player.c"),
                   str(REPO/"main/music_library.c"), str(HERE/"cover_host.c"),
                   str(HERE/"jpeg_mock.c"), str(HERE/"support.c"), "-o", str(dll)]
        if os.name != "nt": command.insert(1, "-fPIC")
        subprocess.run(command, check=True)
        old = Path.cwd()
        os.chdir(temp)
        lib = C.CDLL(str(dll))
        try:
            tests(lib)
        finally:
            lib.lyric_parser_destroy()
            lib.cover_loader_destroy()
            if os.name == "nt":
                import _ctypes
                _ctypes.FreeLibrary(lib._handle)
            os.chdir(old)

if __name__ == "__main__": main()
