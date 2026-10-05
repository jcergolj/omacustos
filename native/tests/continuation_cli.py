#!/usr/bin/env python3
"""Offline CLI subprocess fixture for the public worker integration boundary."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time

root = Path(os.environ["FIXTURE_REMOTE"])
args = sys.argv[1:]
command = args[1]


def remote(path):
    return root / path.lstrip("/")


def metadata(path):
    result = {"type": "file", "size": path.stat().st_size}
    if os.environ.get("FIXTURE_SIZE_ONLY") != "yes":
        with path.open("rb") as contents:
            result["sha256"] = hashlib.file_digest(contents, "sha256").hexdigest()
    return result


def transfer(source, destination):
    if source.is_dir():
        destination.mkdir(exist_ok=True)
        for child in sorted(source.iterdir()):
            transfer(child, destination / child.name)
        return
    if source.name == os.environ.get("FIXTURE_FAIL_ITEM"):
        raise RuntimeError("Connection interrupted")
    shutil.copyfile(source, destination)
    with open(os.environ["FIXTURE_LOG"], "a") as log:
        log.write(json.dumps({"source": str(source), "remote": str(destination), "size": source.stat().st_size}) + "\n")
    if source.name == os.environ.get("FIXTURE_CORRUPT_ITEM"):
        destination.write_bytes(b"!" * source.stat().st_size)
    if source.name == os.environ.get("FIXTURE_BLOCK_ITEM"):
        Path(os.environ["FIXTURE_MARKER"]).write_text(str(destination))
        while True:
            time.sleep(0.05)


try:
    if command == "create-folder":
        (remote(args[2]) / args[3]).mkdir()
    elif command == "list":
        folder = remote(args[-1])
        if not folder.is_dir():
            raise RuntimeError("Folder not found")
        deleted_source = os.environ.get("FIXTURE_DELETE_SOURCE")
        if deleted_source:
            Path(deleted_source).unlink(missing_ok=True)
        values = []
        for path in folder.iterdir():
            values.append({"name": path.name, **({"type": "folder"} if path.is_dir() else metadata(path))})
        print(json.dumps(values))
    elif command == "info":
        path = remote(args[-1])
        if not path.is_file():
            raise RuntimeError("Node not found")
        print(json.dumps(metadata(path)))
    elif command == "upload":
        source = Path(args[-2])
        if source.is_dir():
            files = [p for p in source.rglob("*") if p.is_file()]
            with open(os.environ["FIXTURE_BATCH_LOG"], "a") as log:
                log.write(json.dumps({"bytes": sum(p.stat().st_size for p in files), "files": len(files), "source": str(source)}) + "\n")
        transfer(source, remote(args[-1]) / source.name)
        print("{}")
    elif command == "download":
        shutil.copyfile(remote(args[-2]), Path(args[-1]) / Path(args[-2]).name)
        print("{}")
    elif command in ("trash", "delete"):
        raise RuntimeError("Cleanup must not be invoked by these integration fixtures")
    else:
        raise RuntimeError("Unsupported fixture command")
except Exception as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
