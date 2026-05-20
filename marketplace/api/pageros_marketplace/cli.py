"""Small CLI:

* ``pageros-marketplace validate path/to/manifest.yaml``
* ``pageros-marketplace dump-openapi [--output openapi.json]``
* ``pageros-marketplace serve [--host 0.0.0.0] [--port 8080]``
"""

from __future__ import annotations

import argparse
import json
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

    p_openapi = sub.add_parser(
        "dump-openapi",
        help="Print (or write) the registry REST API's OpenAPI 3.x document.",
    )
    p_openapi.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Path to write openapi.json. Default: stdout.",
    )

    p_serve = sub.add_parser(
        "serve",
        help="Run the registry REST API (requires the 'serve' extra).",
    )
    p_serve.add_argument("--host", default="127.0.0.1")
    p_serve.add_argument("--port", type=int, default=8080)

    args = parser.parse_args(argv)
    if args.cmd == "validate":
        return _cmd_validate(args.path)
    if args.cmd == "dump-openapi":
        return _cmd_dump_openapi(args.output)
    if args.cmd == "serve":
        return _cmd_serve(args.host, args.port)
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


def _cmd_dump_openapi(output: Path | None) -> int:
    from .registry import create_app

    schema = create_app().openapi()
    payload = json.dumps(schema, indent=2, sort_keys=True) + "\n"
    if output is None:
        sys.stdout.write(payload)
    else:
        output.write_text(payload, encoding="utf-8")
        print(f"wrote {output}")
    return 0


def _cmd_serve(host: str, port: int) -> int:  # pragma: no cover - thin wrapper
    try:
        import uvicorn
    except ImportError:
        print(
            "uvicorn is not installed. Re-install with the 'serve' extra:\n"
            "  pip install -e '.[serve]'",
            file=sys.stderr,
        )
        return 2
    from .registry import create_app

    uvicorn.run(create_app(), host=host, port=port)
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
