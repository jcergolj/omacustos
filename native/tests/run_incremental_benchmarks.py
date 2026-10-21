#!/usr/bin/env python3
"""Repeat local-only v3/v4 comparisons; never use configured/cloud backup roots."""
import argparse
from pathlib import Path
import subprocess
import os

parser = argparse.ArgumentParser()
parser.add_argument("binary", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--repeats", type=int, default=2)
parser.add_argument("--bytes", type=int, default=1073741824)
parser.add_argument("--files", type=int, default=4096)
parser.add_argument("--observer", type=Path)
parser.add_argument("--unconstrained", action="store_true", help="Exploratory sizing only; default uses production staging/file-count limits")
arguments = parser.parse_args()
if arguments.repeats < 1:
    parser.error("--repeats must be positive")
arguments.output.mkdir(parents=True, exist_ok=True)
environment = os.environ.copy()
if arguments.observer:
    environment["LD_PRELOAD"] = str(arguments.observer.resolve())
    environment["OMACUSTOS_REQUIRE_STAGING_OBSERVER"] = "1"
for profile in ("repository-files", "repository-code", "mixed"):
    for target in (64000000, 256000000, 1000000000):
        for repeat in range(arguments.repeats):
            destination = arguments.output / f"{profile}-{target}-{repeat + 1}.jsonl"
            # Refuse overwrites: evidence from a previous invocation is not ours
            # to discard, even if a previous measurement failed part-way through.
            with destination.open("x") as output:
                subprocess.run([str(arguments.binary.resolve()), str(arguments.bytes),
                                str(arguments.files), str(target), profile if arguments.unconstrained else "production-" + profile],
                               stdout=output, check=True, env=environment)
            print(destination.name, flush=True)
