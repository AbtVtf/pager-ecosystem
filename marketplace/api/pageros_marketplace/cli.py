"""Tiny CLI: `pageros-marketplace validate path/to/manifest.yaml`."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .manifest import ManifestValidationError, load_manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pageros-marketplace",
        description="PagerOS marketplace tooling.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_validate = sub.add_parser(
        "validate",
        help="Validate a PagerOS app manifest YAML file against the SPEC §10.2 schema.",
    )
    p_validate.add_argument("path", type=Path, help="Path to manifest.yaml")

    args = parser.parse_args(argv)
    if args.cmd == "validate":
        return _cmd_validate(args.path)
    parser.error(f"unknown command: {args.cmd}")
    return 2  # unreachable, parser.error exits


def _cmd_validate(path: Path) -> int:
    try:
        manifest = load_manifest(path)
    except ManifestValidationError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(f"ok: {manifest.id} (v{manifest.version})")
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
