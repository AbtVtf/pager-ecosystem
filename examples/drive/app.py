"""PagerOS Drive — a tiny cloud file storage browser.

Backs onto a local directory (``./drive_store/`` next to this file,
override with ``DRIVE_ROOT``) and exposes browse / view / upload /
mkdir / delete screens. Mirrors the shape of ``examples/notes`` but
swaps the JSON blob for a real filesystem tree.

All filesystem operations route through :func:`safe_path`, which
resolves the requested relative path under the configured root and
refuses anything that climbs above it (``../`` escapes, absolute
paths, symlink targets outside the root, etc.). A missing check here
would be a remote file-write vuln, so every handler must call it
before touching the disk.
"""

from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Any

from pageros import (
    App,
    Button,
    Form,
    Input,
    List,
    ListItem,
    Notification,
    Screen,
    Text,
)


# --------------------------------------------------------------------------- #
# Storage root + path safety
# --------------------------------------------------------------------------- #

DEFAULT_ROOT = Path(__file__).with_name("drive_store")
ROOT = Path(os.environ.get("DRIVE_ROOT", str(DEFAULT_ROOT))).resolve()
ROOT.mkdir(parents=True, exist_ok=True)

TEXT_PREVIEW_LIMIT = 4 * 1024  # 4 KB — anything bigger is treated as binary.
UPLOAD_MAX = 4096


class PathEscapeError(ValueError):
    """Raised when a requested path resolves outside the storage root."""


def safe_path(rel: str) -> Path:
    """Resolve ``rel`` relative to :data:`ROOT`, rejecting any escape.

    Strips a leading ``/`` so query-string paths like ``?p=/foo`` land
    under the root, refuses absolute paths after that strip, then
    resolves and verifies the result is still inside ``ROOT``. Raises
    :class:`PathEscapeError` otherwise — never returns a path outside.
    """
    if rel is None:
        rel = "/"
    rel = rel.strip()
    if not rel:
        rel = "/"
    # Allow leading "/" as the "root of the drive" sentinel from URLs.
    stripped = rel.lstrip("/")
    candidate = (ROOT / stripped).resolve() if stripped else ROOT
    # ``Path.is_relative_to`` is the right primitive (Py3.9+). Fall back
    # to a string-prefix check for safety on older interpreters.
    try:
        inside = candidate == ROOT or candidate.is_relative_to(ROOT)
    except AttributeError:  # pragma: no cover - Py<3.9
        inside = (
            str(candidate) == str(ROOT)
            or str(candidate).startswith(str(ROOT) + os.sep)
        )
    if not inside:
        raise PathEscapeError(f"path escapes drive root: {rel!r}")
    return candidate


def _rel_from_root(p: Path) -> str:
    """Render an absolute path back as a ``/``-prefixed drive path."""
    try:
        rel = p.relative_to(ROOT)
    except ValueError:
        return "/"
    s = str(rel).replace(os.sep, "/")
    if s in ("", "."):
        return "/"
    return "/" + s


def _parent_of(rel: str) -> str:
    if rel in ("", "/", None):
        return "/"
    parent = "/".join(rel.rstrip("/").split("/")[:-1])
    return parent or "/"


def _join_rel(parent: str, name: str) -> str:
    parent = parent or "/"
    if parent == "/":
        return "/" + name
    return parent.rstrip("/") + "/" + name


def _heading_for(rel: str) -> str:
    if rel in ("", "/"):
        return "/"
    return rel.rstrip("/").split("/")[-1] or "/"


def _fmt_size(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n / (1024 * 1024):.1f} MB"


def _fmt_mtime_short(ts: float) -> str:
    return time.strftime("%m-%d %H:%M", time.localtime(ts))


def _fmt_mtime(ts: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _error_screen(msg: str, *, back: str = "/") -> Screen:
    return Screen(
        id="drive_error",
        title="Drive",
        body=[
            Notification(s=msg, level="error"),
            Button(label="Back", href=f"/browse?p={back}", method="GET"),
        ],
        ttl=0,
    )


# --------------------------------------------------------------------------- #
# App + routes
# --------------------------------------------------------------------------- #


app = App(name="drive")


@app.screen("/")
def home(_request=None):
    return _browse_screen("/")


@app.screen("/browse")
def browse(request):
    rel = _first_q(request, "p") or "/"
    return _browse_screen(rel)


def _browse_screen(rel: str) -> Screen:
    try:
        target = safe_path(rel)
    except PathEscapeError as exc:
        return _error_screen(str(exc))
    if not target.exists():
        return _error_screen(f"Not found: {rel}")
    if not target.is_dir():
        # Browsing a file path just redirects to /view.
        return _view_screen(rel)

    entries: list[ListItem] = []
    try:
        children = sorted(target.iterdir(), key=lambda p: p.name.lower())
    except OSError as exc:
        return _error_screen(f"Read failed: {exc}")

    folders = [c for c in children if c.is_dir()]
    files = [c for c in children if c.is_file()]

    for d in folders:
        entries.append(
            ListItem(
                label=d.name,
                sub="folder",
                href=f"/browse?p={_join_rel(rel, d.name)}",
            )
        )
    for f in files:
        try:
            st = f.stat()
            sub = f"{_fmt_size(st.st_size)} · {_fmt_mtime_short(st.st_mtime)}"
        except OSError:
            sub = "?"
        entries.append(
            ListItem(
                label=f.name,
                sub=sub,
                href=f"/view?p={_join_rel(rel, f.name)}",
            )
        )

    body: list[Any] = [Text(s=_heading_for(rel), style="heading")]
    if entries:
        body.append(List(items=entries))
    else:
        body.append(Text(s="(empty)", style="dim"))
    parent = _parent_of(rel)
    if rel not in ("", "/"):
        body.append(Button(label="Up", href=f"/browse?p={parent}", method="GET"))
    body.append(Button(label="New folder", href=f"/mkdir?p={rel}", method="GET"))
    body.append(Button(label="Upload", href=f"/upload?p={rel}", method="GET"))
    return Screen(id="drive_browse", title="Drive", body=body, ttl=0)


@app.screen("/view")
def view(request):
    rel = _first_q(request, "p") or "/"
    return _view_screen(rel)


def _view_screen(rel: str) -> Screen:
    try:
        target = safe_path(rel)
    except PathEscapeError as exc:
        return _error_screen(str(exc))
    if not target.exists() or not target.is_file():
        return _error_screen(f"Not a file: {rel}")
    try:
        st = target.stat()
    except OSError as exc:
        return _error_screen(f"Stat failed: {exc}")

    parent = _parent_of(rel)
    body: list[Any] = [
        Text(s=target.name, style="heading"),
        Text(s=f"{st.st_size} bytes · {_fmt_mtime(st.st_mtime)}", style="dim"),
    ]

    preview: str | None = None
    if st.st_size <= TEXT_PREVIEW_LIMIT:
        try:
            raw = target.read_bytes()
            preview = raw.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            preview = None

    if preview is not None and preview:
        body.append(Text(s=preview, style="mono"))
    elif preview == "":
        body.append(Text(s="(empty file)", style="dim"))
    else:
        body.append(Text(s=f"Binary file ({st.st_size} bytes)", style="dim"))

    body.append(
        Button(
            label="Delete",
            href=f"/delete?p={rel}",
            method="POST",
            confirm="Are you sure?",
        )
    )
    body.append(Button(label="Back", href=f"/browse?p={parent}", method="GET"))
    return Screen(id="drive_view", title="Drive", body=body, ttl=0)


@app.screen("/upload")
def upload_form(request):
    rel = _first_q(request, "p") or "/"
    try:
        target = safe_path(rel)
    except PathEscapeError as exc:
        return _error_screen(str(exc))
    if not target.is_dir():
        return _error_screen(f"Not a folder: {rel}")
    return Screen(
        id="drive_upload",
        title="Upload",
        body=[
            Text(s=f"Upload to {_heading_for(rel)}", style="heading"),
            Form(
                action=f"/upload?p={rel}",
                method="POST",
                submit="Save",
                fields=[
                    Input(name="filename", label="Filename", max=120),
                    Input(name="content", label="Content", max=UPLOAD_MAX),
                ],
            ),
            Button(label="Cancel", href=f"/browse?p={rel}", method="GET"),
        ],
        ttl=0,
    )


@app.handler("/upload", method="POST")
def upload_save(request):
    rel = _first_q(request, "p") or "/"
    body = request.body if isinstance(request.body, dict) else {}
    filename = (body.get("filename") or "").strip()
    content = body.get("content") or ""
    if not filename:
        return _error_screen("Filename required.", back=rel)
    # Reject path separators in the filename — uploads must land in
    # the requested folder, not somewhere else under the root.
    if "/" in filename or "\\" in filename or filename in (".", ".."):
        return _error_screen("Invalid filename.", back=rel)
    try:
        folder = safe_path(rel)
        target = safe_path(_join_rel(rel, filename))
    except PathEscapeError as exc:
        return _error_screen(str(exc), back="/")
    if not folder.is_dir():
        return _error_screen(f"Not a folder: {rel}", back="/")
    if target.exists():
        return _error_screen(f"Already exists: {filename}", back=rel)
    if isinstance(content, bytes):
        data = content
    else:
        data = str(content).encode("utf-8")
    try:
        target.write_bytes(data)
    except OSError as exc:
        return _error_screen(f"Write failed: {exc}", back=rel)
    return _browse_screen(rel)


@app.screen("/mkdir")
def mkdir_form(request):
    rel = _first_q(request, "p") or "/"
    try:
        target = safe_path(rel)
    except PathEscapeError as exc:
        return _error_screen(str(exc))
    if not target.is_dir():
        return _error_screen(f"Not a folder: {rel}")
    return Screen(
        id="drive_mkdir",
        title="New folder",
        body=[
            Text(s=f"New folder in {_heading_for(rel)}", style="heading"),
            Form(
                action=f"/mkdir?p={rel}",
                method="POST",
                submit="Create",
                fields=[Input(name="name", label="Name", max=120)],
            ),
            Button(label="Cancel", href=f"/browse?p={rel}", method="GET"),
        ],
        ttl=0,
    )


@app.handler("/mkdir", method="POST")
def mkdir_create(request):
    rel = _first_q(request, "p") or "/"
    body = request.body if isinstance(request.body, dict) else {}
    name = (body.get("name") or "").strip()
    if not name:
        return _error_screen("Name required.", back=rel)
    if "/" in name or "\\" in name or name in (".", ".."):
        return _error_screen("Invalid folder name.", back=rel)
    try:
        parent = safe_path(rel)
        target = safe_path(_join_rel(rel, name))
    except PathEscapeError as exc:
        return _error_screen(str(exc), back="/")
    if not parent.is_dir():
        return _error_screen(f"Not a folder: {rel}", back="/")
    if target.exists():
        return _error_screen(f"Already exists: {name}", back=rel)
    try:
        target.mkdir(parents=False, exist_ok=False)
    except OSError as exc:
        return _error_screen(f"Mkdir failed: {exc}", back=rel)
    return _browse_screen(rel)


@app.handler("/delete", method="POST")
def delete(request):
    rel = _first_q(request, "p") or "/"
    try:
        target = safe_path(rel)
    except PathEscapeError as exc:
        return _error_screen(str(exc))
    if target == ROOT:
        return _error_screen("Cannot delete drive root.")
    if not target.exists():
        return _error_screen(f"Not found: {rel}")
    parent = _parent_of(rel)
    try:
        if target.is_file():
            target.unlink()
        elif target.is_dir():
            try:
                target.rmdir()
            except OSError:
                return _error_screen("Folder not empty.", back=rel)
        else:
            return _error_screen("Unsupported entry type.", back=parent)
    except OSError as exc:
        return _error_screen(f"Delete failed: {exc}", back=parent)
    return _browse_screen(parent)


if __name__ == "__main__":
    app.run()
