"""CLI entry point for the marketplace web UI."""

from __future__ import annotations

import argparse
import sys


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pageros-marketplace-web",
        description="PagerOS marketplace public web UI.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_serve = sub.add_parser("serve", help="Run the web UI against an in-memory Registry.")
    p_serve.add_argument("--host", default="127.0.0.1")
    p_serve.add_argument("--port", type=int, default=8081)

    args = parser.parse_args(argv)
    if args.cmd == "serve":
        return _cmd_serve(args.host, args.port)
    parser.error(f"unknown command: {args.cmd}")
    return 2


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
    from pageros_marketplace.registry.store import Registry

    from .app import create_app

    uvicorn.run(create_app(Registry()), host=host, port=port)
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
