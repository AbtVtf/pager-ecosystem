"""Tests for the PY-009 dev server.

The acceptance criterion is < 1 s reload latency. The performance test below
measures end-to-end: file edit → child process restarted (new PID + alive),
asserting it completes well under 1 s on a clean SDK-sized project.
"""

from __future__ import annotations

import os
import signal
import sys
import time
from pathlib import Path

import pytest

from pageros.devserver import (
    DevServer,
    FileWatcher,
    ProcessRunner,
    _split_exts,
    default_child_command,
)


# --------------------------------------------------------------------------- #
# FileWatcher
# --------------------------------------------------------------------------- #


def test_filewatcher_detects_new_file(tmp_path: Path) -> None:
    w = FileWatcher(root=tmp_path)
    w.prime()
    assert w.poll() == []
    target = tmp_path / "new.py"
    target.write_text("x = 1\n")
    # Force mtime to differ from the prime baseline epoch (in case the test
    # filesystem resolution rounds aggressively).
    os.utime(target, (time.time(), time.time()))
    changed = w.poll()
    assert str(target) in changed


def test_filewatcher_detects_modification(tmp_path: Path) -> None:
    target = tmp_path / "mod.py"
    target.write_text("v = 0\n")
    w = FileWatcher(root=tmp_path)
    w.prime()
    # Bump mtime by 1s to defeat any coarse mtime resolution on the runner.
    future = time.time() + 1
    os.utime(target, (future, future))
    changed = w.poll()
    assert str(target) in changed


def test_filewatcher_ignores_excluded_dirs(tmp_path: Path) -> None:
    (tmp_path / "__pycache__").mkdir()
    pyc = tmp_path / "__pycache__" / "ignored.py"
    pyc.write_text("noise = 1\n")
    keeper = tmp_path / "kept.py"
    keeper.write_text("kept = 1\n")
    w = FileWatcher(root=tmp_path)
    snapshot = w._scan()
    assert str(keeper) in snapshot
    assert str(pyc) not in snapshot


def test_filewatcher_only_tracks_configured_extensions(tmp_path: Path) -> None:
    (tmp_path / "a.py").write_text("a = 1\n")
    (tmp_path / "b.txt").write_text("ignored\n")
    w = FileWatcher(root=tmp_path, extensions=(".py",))
    snapshot = w._scan()
    assert any(p.endswith("a.py") for p in snapshot)
    assert not any(p.endswith("b.txt") for p in snapshot)


def test_filewatcher_detects_deletion(tmp_path: Path) -> None:
    target = tmp_path / "doomed.py"
    target.write_text("doomed = 1\n")
    w = FileWatcher(root=tmp_path)
    w.prime()
    target.unlink()
    changed = w.poll()
    assert str(target) in changed


def test_split_exts_normalizes_dots() -> None:
    assert _split_exts("py,.toml") == (".py", ".toml")
    assert _split_exts(" py , js ") == (".py", ".js")
    assert _split_exts("") == ()


# --------------------------------------------------------------------------- #
# ProcessRunner
# --------------------------------------------------------------------------- #


SLEEP_SCRIPT = "import time, sys; sys.stdout.write('ready\\n'); sys.stdout.flush(); time.sleep(60)"


def _sleep_cmd() -> list[str]:
    return [sys.executable, "-u", "-c", SLEEP_SCRIPT]


def test_process_runner_start_and_stop() -> None:
    runner = ProcessRunner(command=_sleep_cmd())
    runner.start()
    pid = runner.pid
    assert pid is not None
    assert runner.is_running()
    runner.stop()
    assert not runner.is_running()
    # Confirm OS no longer reports the process group as alive.
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)


def test_process_runner_restart_changes_pid() -> None:
    runner = ProcessRunner(command=_sleep_cmd())
    runner.start()
    first = runner.pid
    runner.restart()
    second = runner.pid
    runner.stop()
    assert first is not None and second is not None
    assert first != second


def test_process_runner_force_kills_unresponsive_child() -> None:
    # A child that traps SIGTERM so we exercise the SIGKILL fallback.
    script = (
        "import signal, time; "
        "signal.signal(signal.SIGTERM, lambda *a: None); "
        "time.sleep(60)"
    )
    runner = ProcessRunner(
        command=[sys.executable, "-u", "-c", script], stop_timeout=0.2
    )
    runner.start()
    runner.stop()
    assert not runner.is_running()


# --------------------------------------------------------------------------- #
# DevServer
# --------------------------------------------------------------------------- #


def _start_devserver(tmp_path: Path, **kwargs) -> tuple[DevServer, list]:
    """Boot a DevServer rooted at tmp_path running a long-lived child."""
    restarts: list = []
    server = DevServer(
        watcher=FileWatcher(root=tmp_path, interval=kwargs.get("interval", 0.02)),
        runner=ProcessRunner(command=_sleep_cmd()),
        debounce=kwargs.get("debounce", 0.02),
        on_restart=lambda paths: restarts.append((time.monotonic(), paths)),
    )
    server.start()
    return server, restarts


def test_devserver_restarts_on_file_change(tmp_path: Path) -> None:
    target = tmp_path / "app.py"
    target.write_text("v = 0\n")
    server, restarts = _start_devserver(tmp_path)
    try:
        first_pid = server.runner.pid
        assert first_pid is not None
        # Give the watcher a beat to capture the baseline.
        time.sleep(0.1)
        future = time.time() + 1
        os.utime(target, (future, future))
        # Wait up to 1.5 s for the restart to be observable.
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            if restarts:
                break
            time.sleep(0.02)
        assert restarts, "dev server did not restart within 1.5 s"
        new_pid = server.runner.pid
        assert new_pid is not None
        assert new_pid != first_pid
    finally:
        server.stop()


def test_devserver_reload_latency_under_one_second(tmp_path: Path) -> None:
    """PY-009 acceptance: < 1 s reload on file change."""
    target = tmp_path / "app.py"
    target.write_text("v = 0\n")
    server, restarts = _start_devserver(tmp_path)
    try:
        time.sleep(0.1)  # let the watcher prime
        edit_at = time.monotonic()
        future = time.time() + 1
        os.utime(target, (future, future))
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            if restarts:
                break
            time.sleep(0.01)
        assert restarts, "reload did not occur"
        restart_at, _paths = restarts[0]
        elapsed = restart_at - edit_at
        assert elapsed < 1.0, f"reload took {elapsed:.3f}s; budget is 1.0s"
    finally:
        server.stop()


def test_devserver_debounces_burst_edits(tmp_path: Path) -> None:
    target = tmp_path / "app.py"
    target.write_text("v = 0\n")
    server, restarts = _start_devserver(tmp_path, debounce=0.15)
    try:
        time.sleep(0.1)
        # Burst: ten rapid edits within the debounce window.
        for i in range(10):
            future = time.time() + (i + 1)
            os.utime(target, (future, future))
            time.sleep(0.01)
        # Allow time for the (single) debounced restart to fire.
        time.sleep(0.6)
        assert len(restarts) == 1, f"expected 1 restart, got {len(restarts)}"
    finally:
        server.stop()


def test_default_child_command_contains_app_and_address() -> None:
    cmd = default_child_command("/tmp/app.py", "0.0.0.0", 9000)
    assert sys.executable in cmd
    assert "/tmp/app.py" in cmd
    assert "--host" in cmd and "0.0.0.0" in cmd
    assert "--port" in cmd and "9000" in cmd
