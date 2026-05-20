#!/usr/bin/env python3
"""Drive the PagerOS CBOR codec against every PROTO-003 vector.

For each vector listed in `protocol/test-vectors/ui/index.json`, invoke the
`cbor_roundtrip` host binary with the `.cbor` payload, capture its stdout,
and assert it byte-equals the original payload. A pass demonstrates that
the codec decodes the bytes into its value tree and re-emits exactly the
same canonical CBOR.

Note on `kind == "negative"` vectors: these contain *valid* CBOR whose
*application-level* meaning the SDK is required to reject (unknown frame
version, top-level not a map, etc.). Frame-shape validation lives one
layer above the codec (see SPEC.md §7 / the future PROTO-005 conformance
runner), so for FW-016 the codec is expected to round-trip them like any
other vector — and `cbor_roundtrip` does exactly that.

The script is pure stdlib so it runs anywhere `python3` is available; no
need for an ESP-IDF toolchain or hardware.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run_one(binary: Path, vec_dir: Path, vec: dict) -> tuple[bool, str]:
    cbor_path = vec_dir / vec["files"]["cbor"]
    proc = subprocess.run([str(binary), str(cbor_path)], capture_output=True)
    if proc.returncode != 0:
        return False, f"exit {proc.returncode}\n{proc.stderr.decode(errors='replace')}"

    expected = cbor_path.read_bytes()
    if proc.stdout != expected:
        return False, (f"byte mismatch (expected {len(expected)} B, got {len(proc.stdout)} B)\n"
                       f"expected: {expected.hex()}\n"
                       f"actual:   {proc.stdout.hex()}")
    return True, "ok"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--vectors-dir", required=True, type=Path)
    p.add_argument("--only", help="substring filter on vector name")
    args = p.parse_args()

    index = json.loads((args.vectors_dir / "index.json").read_text())

    failures: list[tuple[str, str]] = []
    by_kind: dict[str, int] = {}

    for vec in index["vectors"]:
        if args.only and args.only not in vec["name"]:
            continue
        ok, msg = run_one(args.binary, args.vectors_dir, vec)
        by_kind[vec["kind"]] = by_kind.get(vec["kind"], 0) + 1
        if not ok:
            failures.append((vec["name"], msg))

    total = sum(by_kind.values())
    print(f"vectors: {total}  by-kind: {by_kind}")
    if failures:
        print(f"FAILED: {len(failures)}")
        for name, msg in failures:
            print(f"  - {name}")
            for line in msg.splitlines():
                print(f"      {line}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
