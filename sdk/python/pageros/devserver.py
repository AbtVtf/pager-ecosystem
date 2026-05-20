"""Dev server with auto-reload (PY-009).

Watches a project directory and restarts a child process whenever a watched
file changes. Designed so `pagerctl dev` (CLI-002) can spawn it and meet the
< 1 s reload acceptance criterion.

No third-party dependencies: file watching uses ``os.scandir`` mtime polling
on a short interval (default 50 ms).
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

log = logging.getLogger("pageros.devserver")

DEFAULT_EXTENSIONS = (".py",)
DEFAULT_IGNORED_DIRS = frozenset({
    "__pycache__",
    ".git",
    ".venv",
    "venv",
    "node_modules",
    "dist",
    "build",
    ".pytest_cache",
    ".mypy_cache",
})


# --------------------------------------------------------------------------- #
# File watcher
# --------------------------------------------------------------------------- #


@dataclass
class FileWatcher:
    """Polls a directory tree for file changes.

    The watcher tracks per-file mtimes. On the first scan it records the
    baseline; subsequent scans report any path whose mtime advanced or that
    appeared/disappeared. A 50 ms interval keeps poll overhead well under the
    1 s reload budget while staying responsive on typical SDK projects.
    """

    root: Path
    extensions: tuple[str, ...] = DEFAULT_EXTENSIONS
    ignored_dirs: frozenset[str] = DEFAULT_IGNORED_DIRS
    interval: float = 0.05
    _state: dict[str, float] = field(default_factory=dict)
    _stop: threading.Event = field(default_factory=threading.Event)
    _thread: threading.Thread | None = None

    def __post_init__(self) -> None:
        self.root = Path(self.root).resolve()

    def _scan(self) -> dict[str, float]:
        snapshot: dict[str, float] = {}
        stack = [str(self.root)]
        while stack:
            current = stack.pop()
            try:
                it = os.scandir(current)
            except (FileNotFoundError, PermissionError):
                continue
            with it:
                for entry in it:
                    name = entry.name
                    if entry.is_dir(follow_symlinks=False):
                        if name in self.ignored_dirs or name.startswith("."):
                            continue
                        stack.append(entry.path)
                        continue
                    if not entry.is_file(follow_symlinks=False):
                        continue
                    if self.extensions and not name.endswith(self.extensions):
                        continue
                    try:
                        snapshot[entry.path] = entry.stat().st_mtime
                    except (FileNotFoundError, PermissionError):
                        continue
        return snapshot

    def prime(self) -> None:
        """Record the baseline so subsequent ``poll()`` calls report deltas."""
        self._state = self._scan()

    def poll(self) -> list[str]:
        """Return changed paths since the last ``prime()`` or ``poll()`` call."""
        current = self._scan()
        changed: list[str] = []
        for path, mtime in current.items():
            previous = self._state.get(path)
            if previous is None or mtime != previous:
                changed.append(path)
        for path in self._state:
            if path not in current:
                changed.append(path)
        self._state = current
        return changed

    def watch(self, on_change: Callable[[list[str]], None]) -> None:
        """Run a polling loop on the current thread until ``stop()``.

        ``on_change`` is invoked with the list of changed paths. The baseline
        is primed on entry so the first call only fires for actual changes.
        """
        self.prime()
        while not self._stop.is_set():
            time.sleep(self.interval)
            changed = self.poll()
            if changed:
                on_change(changed)

    def start(self, on_change: Callable[[list[str]], None]) -> None:
        """Run :meth:`watch` in a background daemon thread."""
        if self._thread is not None:
            raise RuntimeError("watcher already started")
        self._stop.clear()
        self._thread = threading.Thread(
            target=self.watch, args=(on_change,), daemon=True, name="pageros-watch"
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None


# --------------------------------------------------------------------------- #
# Process runner
# --------------------------------------------------------------------------- #


@dataclass
class ProcessRunner:
    """Runs a child command, with start/stop/restart and PID inspection."""

    command: list[str]
    cwd: Path | None = None
    env: dict[str, str] | None = None
    stop_signal: int = signal.SIGTERM
    stop_timeout: float = 0.5
    _proc: subprocess.Popen | None = None

    @property
    def pid(self) -> int | None:
        return self._proc.pid if self._proc else None

    def is_running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def start(self) -> None:
        if self.is_running():
            return
        log.info("starting: %s", " ".join(self.command))
        self._proc = subprocess.Popen(
            self.command,
            cwd=str(self.cwd) if self.cwd else None,
            env=self.env,
            start_new_session=True,
        )

    def stop(self) -> None:
        if not self.is_running():
            self._proc = None
            return
        assert self._proc is not None
        try:
            os.killpg(os.getpgid(self._proc.pid), self.stop_signal)
        except ProcessLookupError:
            self._proc = None
            return
        try:
            self._proc.wait(timeout=self.stop_timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self._proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self._proc.wait(timeout=self.stop_timeout)
        self._proc = None

    def restart(self) -> None:
        self.stop()
        self.start()


# --------------------------------------------------------------------------- #
# Dev server
# --------------------------------------------------------------------------- #


@dataclass
class DevServer:
    """Watches a directory and restarts a child command on change.

    The default debounce interval (50 ms) collapses bursts of editor saves
    into a single restart while keeping end-to-end reload latency under the
    PY-009 budget of 1 s on a normal-sized SDK project.
    """

    watcher: FileWatcher
    runner: ProcessRunner
    debounce: float = 0.05
    on_restart: Callable[[list[str]], None] | None = None
    _pending: threading.Event = field(default_factory=threading.Event)
    _stop: threading.Event = field(default_factory=threading.Event)
    _worker: threading.Thread | None = None
    _last_change: list[str] = field(default_factory=list)
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def _handle_change(self, paths: list[str]) -> None:
        with self._lock:
            self._last_change = paths
        self._pending.set()

    def _restart_loop(self) -> None:
        while not self._stop.is_set():
            if not self._pending.wait(timeout=0.1):
                continue
            # Debounce: wait briefly and re-arm if more changes arrive.
            time.sleep(self.debounce)
            self._pending.clear()
            with self._lock:
                paths = list(self._last_change)
            log.info("change detected (%d file(s)) — restarting", len(paths))
            self.runner.restart()
            if self.on_restart is not None:
                try:
                    self.on_restart(paths)
                except Exception:  # pragma: no cover - user hook
                    log.exception("on_restart hook raised")

    def start(self) -> None:
        self.runner.start()
        self.watcher.start(self._handle_change)
        self._stop.clear()
        self._worker = threading.Thread(
            target=self._restart_loop, daemon=True, name="pageros-restart"
        )
        self._worker.start()

    def stop(self) -> None:
        self._stop.set()
        self._pending.set()  # unblock the worker
        if self._worker is not None:
            self._worker.join(timeout=2.0)
            self._worker = None
        self.watcher.stop()
        self.runner.stop()

    def run_forever(self) -> int:
        """Convenience wrapper for ``__main__`` use: blocks on SIGINT/SIGTERM."""
        self.start()
        try:
            stopping = threading.Event()

            def _signal_handler(signum: int, _frame) -> None:
                log.info("received signal %d, shutting down", signum)
                stopping.set()

            signal.signal(signal.SIGINT, _signal_handler)
            signal.signal(signal.SIGTERM, _signal_handler)
            while not stopping.is_set():
                stopping.wait(timeout=0.5)
        finally:
            self.stop()
        return 0


# --------------------------------------------------------------------------- #
# Default child command
# --------------------------------------------------------------------------- #


def default_child_command(app_path: str, host: str, port: int) -> list[str]:
    """Build the default subprocess command for serving ``app_path``.

    ``app_path`` is a filesystem path to a Python module that, when executed,
    starts a server bound to ``host:port``. The dev server itself stays
    agnostic to the SDK's eventual HTTP runtime (PY-001 et al.).
    """
    return [
        sys.executable,
        "-u",
        str(app_path),
        "--host",
        host,
        "--port",
        str(port),
    ]


# --------------------------------------------------------------------------- #
# CLI entry point
# --------------------------------------------------------------------------- #


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pageros-dev",
        description="Run a PagerOS app with auto-reload on file change.",
    )
    parser.add_argument(
        "--app",
        required=True,
        help="Path to a Python module that starts the app server.",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument(
        "--watch",
        default=None,
        help="Directory to watch (defaults to the app's directory).",
    )
    parser.add_argument(
        "--ext",
        default=",".join(DEFAULT_EXTENSIONS),
        help="Comma-separated list of file extensions to watch.",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.05,
        help="Polling interval in seconds (default: 0.05).",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    return parser


def _split_exts(value: str) -> tuple[str, ...]:
    parts = [p.strip() for p in value.split(",") if p.strip()]
    return tuple(p if p.startswith(".") else f".{p}" for p in parts)


def main(argv: Iterable[str] | None = None) -> int:
    args = _build_parser().parse_args(list(argv) if argv is not None else None)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    app_path = Path(args.app).resolve()
    if not app_path.exists():
        print(f"error: app path does not exist: {app_path}", file=sys.stderr)
        return 2
    watch_root = Path(args.watch).resolve() if args.watch else app_path.parent
    watcher = FileWatcher(
        root=watch_root,
        extensions=_split_exts(args.ext),
        interval=args.interval,
    )
    runner = ProcessRunner(
        command=default_child_command(str(app_path), args.host, args.port),
        cwd=watch_root,
    )
    server = DevServer(watcher=watcher, runner=runner)
    return server.run_forever()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
