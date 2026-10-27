#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the SAM port is byte-identical to the upstream program.

Builds 3rdparty/SAM (the reference) and tools/sam_parity.c (the port), renders
the same phrases through both, and compares the 8-bit streams sample by
sample. The only allowed difference is at the very start: the reference leaves
its first three bytes uninitialised (malloc garbage, heard as a click after the
output buffer is handed to the sound card), the port starts them at 128.

    python3 tools/sam_parity.py            # full matrix
    python3 tools/sam_parity.py --quick    # a handful
Exit status 0 only when every case matches.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAM = os.path.join(ROOT, "3rdparty", "SAM")
OUT = os.path.join(ROOT, "out", "sam-parity")

# (text, pitch, speed, mouth, throat, sing). The controls are SAM's own, and
# the voice settings are the ones SAM's own demo2 uses to change its voice.
CASES = [
    ("say", 64, 72, 128, 128, 0),
    ("it", 64, 72, 128, 128, 0),
    ("say it", 64, 72, 128, 128, 0),
    ("hello world", 64, 72, 128, 128, 0),
    ("speech synthesis", 64, 72, 128, 128, 0),
    ("the quick brown fox", 64, 72, 128, 128, 0),
    ("i can change my voice completely", 64, 72, 110, 160, 0),
    ("strange alien", 60, 42, 190, 190, 0),
    ("silly voice", 72, 82, 110, 105, 0),
    ("little old lady", 32, 72, 145, 145, 0),
    ("really high", 110, 72, 128, 128, 0),
    ("really low", 30, 72, 128, 128, 0),
    ("bee", 64, 72, 128, 128, 0),
    ("zero one two three four five six seven eight nine", 64, 72, 128, 128, 0),
    ("can you hear me now?", 64, 60, 128, 128, 0),
    ("stop! wait...", 64, 90, 128, 128, 0),
    ("singing", 64, 72, 128, 128, 1),
    ("zoo vision", 64, 72, 128, 128, 0),   # voiced sampled consonants
    ("judge them", 50, 72, 128, 128, 0),
    ("whatever", 20, 127, 60, 200, 0),     # extremes
]

QUICK = CASES[:4]


def build():
    os.makedirs(OUT, exist_ok=True)
    ref = os.path.join(OUT, "sam-reference")
    if not os.path.exists(ref):
        subprocess.run(
            ["gcc", "-O2", "-o", ref,
             "src/reciter.c", "src/sam.c", "src/render.c", "src/main.c",
             "src/debug.c"],
            cwd=SAM, check=True)
    port = os.path.join(OUT, "sam-port")
    subprocess.run(
        ["gcc", "-O2", "-Wall", "-Wextra", "-I", os.path.join(ROOT, "sam"),
         os.path.join(ROOT, "tools", "sam_parity.c"),
         os.path.join(ROOT, "sam", "sam_port.c"),
         os.path.join(ROOT, "sam", "reciter.c"), "-o", port],
        check=True)
    return ref, port


def read_wav(path):
    """Raw 8-bit mono samples, skipping the 44-byte header.

    Not wave.open(): s-macke's WriteWav() writes a RIFF chunk size eight bytes
    short of its own data chunk, so every reader that trusts RIFF truncates the
    last eight samples. The data chunk itself is complete.
    """
    raw = open(path, "rb").read()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", path
    return list(raw[44:])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep the WAVs")
    args = ap.parse_args(argv)
    ref, port = build()
    cases = QUICK if args.quick else CASES
    fails = 0
    for i, (text, pitch, speed, mouth, throat, sing) in enumerate(cases):
        tag = "c%02d" % i
        a = os.path.join(OUT, tag + "-port.wav")
        b = os.path.join(OUT, tag + "-ref.wav")
        subprocess.run([port, a, str(pitch), str(speed), str(mouth),
                        str(throat), str(sing), text], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        args_ref = [ref, "-wav", b, "-pitch", str(pitch), "-speed", str(speed),
                    "-mouth", str(mouth), "-throat", str(throat)]
        if sing:
            args_ref.append("-sing")
        subprocess.run(args_ref + [text], check=True, cwd=SAM,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pa, rb = read_wav(a), read_wav(b)
        n = min(len(pa), len(rb))
        bad = [k for k in range(3, n) if pa[k] != rb[k]]
        # The reference's first three bytes are whatever malloc handed back.
        ok = not bad and len(pa) == len(rb)
        why = "-"
        if bad:
            why = "%d (%d bad of %d)" % (bad[0], len(bad), n - 3)
        elif len(pa) != len(rb):
            why = "length"
        print("%s  %-44s len %d/%d  first-diff %s" % (
            "PASS" if ok else "FAIL", text[:44], len(pa), len(rb), why))
        if not ok:
            fails += 1
    if not args.keep:
        for f in os.listdir(OUT):
            if f.startswith("c") and f.endswith(".wav"):
                os.remove(os.path.join(OUT, f))
    print("%d/%d cases byte-identical" % (len(cases) - fails, len(cases)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
