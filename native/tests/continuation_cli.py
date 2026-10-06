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
if os.environ.get("FIXTURE_COMMAND_LOG"):
    with open(os.environ["FIXTURE_COMMAND_LOG"], "a") as log:
        log.write(json.dumps({"command": command, "args": args}) + "\n")


def remote(path):
    return root / path.lstrip("/")


def metadata(path):
    if path.name.endswith(".tar.gz") and os.environ.get("FIXTURE_ARCHIVE_STORAGE_SIZE_ONLY"):
        return {"type": "file", "totalStorageSize": path.stat().st_size}
    result = {"type": "file", "size": path.stat().st_size}
    if os.environ.get("FIXTURE_SIZE_ONLY") != "yes":
        with path.open("rb") as contents:
            result["sha256"] = hashlib.file_digest(contents, "sha256").hexdigest()
    return result


def block_archive(path, phase):
    if (path.name.endswith(".tar.gz")
            and os.environ.get("FIXTURE_BLOCK_ARCHIVE_PHASE") == phase
            and len(list(root.rglob("*.tar.gz"))) == int(os.environ.get("FIXTURE_BLOCK_ARCHIVE_NUMBER", "2"))):
        Path(os.environ["FIXTURE_MARKER"]).write_text(str(path))
        while True:
            time.sleep(0.05)


def transfer(source, destination):
    if source.is_dir():
        destination.mkdir(exist_ok=True)
        for child in sorted(source.iterdir()):
            transfer(child, destination / child.name)
        return
    if source.name == os.environ.get("FIXTURE_FAIL_ITEM"):
        raise RuntimeError("Connection interrupted")
    if (source.name.endswith(".tar.gz") and os.environ.get("FIXTURE_FAIL_ARCHIVE_NUMBER")
            and len(list(root.rglob("*.tar.gz"))) + 1 == int(os.environ["FIXTURE_FAIL_ARCHIVE_NUMBER"])):
        if os.environ.get("FIXTURE_PARTIAL_ARCHIVE"):
            destination.write_bytes(source.read_bytes()[:source.stat().st_size // 2])
        raise RuntimeError(os.environ.get("FIXTURE_TRANSFER_ERROR", "Connection interrupted"))
    shutil.copyfile(source, destination)
    if source.name.endswith(".tar.gz") and os.environ.get("FIXTURE_REPLACE_SOURCE"):
        changed = Path(os.environ["FIXTURE_REPLACE_SOURCE"])
        changed.unlink()
        changed.write_bytes(b"changed pathname after preparation")
    with open(os.environ["FIXTURE_LOG"], "a") as log:
        log.write(json.dumps({"source": str(source), "remote": str(destination), "size": source.stat().st_size}) + "\n")
    block_archive(destination, "upload")
    if source.name == os.environ.get("FIXTURE_CORRUPT_ITEM"):
        destination.write_bytes(b"!" * source.stat().st_size)
    if source.name.endswith(".tar.gz") and os.environ.get("FIXTURE_CORRUPT_ARCHIVE"):
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
        block_archive(path, "verification")
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
        # Explicit writes let the quota fixture exercise the real download sink,
        # instead of bypassing it through copy_file_range/sendfile.
        with remote(args[-2]).open("rb") as source, (Path(args[-1]) / Path(args[-2]).name).open("wb", buffering=0) as destination:
            while chunk := source.read(64 * 1024):
                destination.write(chunk)
        print("{}")
    elif command in ("trash", "delete"):
        raise RuntimeError("Cleanup must not be invoked by these integration fixtures")
    else:
        raise RuntimeError("Unsupported fixture command")
except Exception as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
