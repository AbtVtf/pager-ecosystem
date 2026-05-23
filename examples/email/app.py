"""PagerOS email example (``email.pageros.app``).

IMAP inbox viewer with SMTP reply. Demonstrates a read-only list-of-things
home screen plus a Form-driven compose flow, with graceful degradation to
a mock inbox when the relevant environment variables are not configured
or the upstream servers are unreachable.

Env vars
--------
``IMAP_HOST`` / ``IMAP_USER`` / ``IMAP_PASS`` / ``IMAP_PORT`` (default 993)
``SMTP_HOST`` / ``SMTP_USER`` / ``SMTP_PASS`` / ``SMTP_PORT`` (default 587)

When ``IMAP_HOST`` / ``IMAP_USER`` / ``IMAP_PASS`` are missing or the
IMAP connection fails, the inbox falls back to a small canned list so
the UI is still navigable on a fresh checkout. Likewise SMTP failures
surface as an error :class:`Notification` rather than a 500.
"""

from __future__ import annotations

import email
import email.header
import email.utils
import imaplib
import logging
import os
import smtplib
import ssl
from email.message import EmailMessage
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

log = logging.getLogger("examples.email")

INBOX_LIMIT = 20
SUBJECT_TRIM = 60
BODY_PREVIEW = 800
IMAP_TIMEOUT = 8.0
SMTP_TIMEOUT = 8.0

# ---- Mock fallback inbox -------------------------------------------------- #

_MOCK_INBOX: list[dict[str, str]] = [
    {
        "uid": "1001",
        "subject": "Welcome to PagerOS Mail",
        "from": "team@pageros.test",
        "date": "Mon, 22 May 2026 09:14",
        "body": (
            "Hi there!\n\nThis is a mock inbox shown because IMAP creds "
            "weren't configured. Set IMAP_HOST/IMAP_USER/IMAP_PASS to "
            "see real mail.\n\n— PagerOS"
        ),
    },
    {
        "uid": "1002",
        "subject": "Your shipment is on the way",
        "from": "noreply@parcels.example",
        "date": "Sun, 21 May 2026 18:02",
        "body": "Tracking #4471. ETA Tuesday.",
    },
    {
        "uid": "1003",
        "subject": "Re: Lunch?",
        "from": "alex@friends.example",
        "date": "Sun, 21 May 2026 12:48",
        "body": "Yeah, noon works. Same place as last time?",
    },
    {
        "uid": "1004",
        "subject": "Invoice #2026-0518",
        "from": "billing@vendor.example",
        "date": "Sat, 20 May 2026 22:00",
        "body": "Attached invoice is due in 30 days.",
    },
    {
        "uid": "1005",
        "subject": "Security alert: new sign-in",
        "from": "security@pageros.test",
        "date": "Sat, 20 May 2026 07:33",
        "body": "We noticed a new sign-in from a pager device.",
    },
]


# ---- Helpers -------------------------------------------------------------- #


def _env(name: str, default: str | None = None) -> str | None:
    v = os.environ.get(name)
    if v is None:
        return default
    v = v.strip()
    return v or default


def _imap_configured() -> bool:
    return all(_env(k) for k in ("IMAP_HOST", "IMAP_USER", "IMAP_PASS"))


def _smtp_configured() -> bool:
    return all(_env(k) for k in ("SMTP_HOST", "SMTP_USER", "SMTP_PASS"))


def _first_q(request, key: str) -> str:
    vals = request.query.get(key) or []
    return (vals[0] if vals else "").strip()


def _trim(s: str, n: int) -> str:
    s = (s or "").strip().replace("\r", " ").replace("\n", " ")
    return s if len(s) <= n else s[: n - 1] + "…"


def _decode_header(raw: str | None) -> str:
    if not raw:
        return ""
    try:
        parts = email.header.decode_header(raw)
    except Exception:
        return raw
    out: list[str] = []
    for chunk, enc in parts:
        if isinstance(chunk, bytes):
            try:
                out.append(chunk.decode(enc or "utf-8", errors="replace"))
            except (LookupError, TypeError):
                out.append(chunk.decode("utf-8", errors="replace"))
        else:
            out.append(chunk)
    return "".join(out).strip()


def _date_short(raw: str | None) -> str:
    if not raw:
        return ""
    try:
        dt = email.utils.parsedate_to_datetime(raw)
    except (TypeError, ValueError):
        return raw[:16]
    if dt is None:
        return raw[:16]
    return dt.strftime("%b %d %H:%M")


def _from_addr(raw: str | None) -> str:
    if not raw:
        return "(unknown)"
    decoded = _decode_header(raw)
    name, addr = email.utils.parseaddr(decoded)
    return addr or name or decoded


# ---- IMAP / SMTP plumbing ------------------------------------------------- #


def _imap_connect() -> imaplib.IMAP4_SSL:
    host = _env("IMAP_HOST") or ""
    port = int(_env("IMAP_PORT") or "993")
    user = _env("IMAP_USER") or ""
    pwd = _env("IMAP_PASS") or ""
    ctx = ssl.create_default_context()
    conn = imaplib.IMAP4_SSL(host, port, ssl_context=ctx, timeout=IMAP_TIMEOUT)
    conn.login(user, pwd)
    return conn


def _fetch_inbox() -> list[dict[str, str]]:
    """Return up to ``INBOX_LIMIT`` most-recent message headers."""
    if not _imap_configured():
        return list(_MOCK_INBOX)
    try:
        conn = _imap_connect()
    except (imaplib.IMAP4.error, OSError, ValueError) as exc:
        log.warning("IMAP connect failed: %s", exc)
        return list(_MOCK_INBOX)
    try:
        conn.select("INBOX", readonly=True)
        typ, data = conn.uid("search", None, "ALL")
        if typ != "OK" or not data or not data[0]:
            return []
        uids = data[0].split()[-INBOX_LIMIT:][::-1]  # newest first
        rows: list[dict[str, str]] = []
        for uid in uids:
            try:
                typ, msg_data = conn.uid(
                    "fetch",
                    uid,
                    "(BODY.PEEK[HEADER.FIELDS (SUBJECT FROM DATE)])",
                )
            except imaplib.IMAP4.error as exc:
                log.warning("IMAP fetch header for uid %s failed: %s", uid, exc)
                continue
            if typ != "OK" or not msg_data:
                continue
            raw_header = b""
            for part in msg_data:
                if isinstance(part, tuple) and len(part) >= 2:
                    raw_header = part[1] or b""
                    break
            try:
                msg = email.message_from_bytes(raw_header)
            except Exception:
                continue
            rows.append(
                {
                    "uid": uid.decode("ascii", errors="replace"),
                    "subject": _decode_header(msg.get("Subject")) or "(no subject)",
                    "from": _from_addr(msg.get("From")),
                    "date": _decode_header(msg.get("Date")) or "",
                }
            )
        return rows
    except (imaplib.IMAP4.error, OSError) as exc:
        log.warning("IMAP inbox listing failed: %s", exc)
        return list(_MOCK_INBOX)
    finally:
        try:
            conn.logout()
        except Exception:
            pass


def _fetch_message(uid: str) -> dict[str, str] | None:
    """Return the full message for ``uid`` or ``None`` if it can't be loaded."""
    if not _imap_configured():
        for m in _MOCK_INBOX:
            if m["uid"] == uid:
                return dict(m)
        return None
    try:
        conn = _imap_connect()
    except (imaplib.IMAP4.error, OSError, ValueError) as exc:
        log.warning("IMAP connect failed: %s", exc)
        for m in _MOCK_INBOX:
            if m["uid"] == uid:
                return dict(m)
        return None
    try:
        conn.select("INBOX", readonly=True)
        typ, data = conn.uid("fetch", uid.encode("ascii"), "(RFC822)")
        if typ != "OK" or not data:
            return None
        raw = b""
        for part in data:
            if isinstance(part, tuple) and len(part) >= 2:
                raw = part[1] or b""
                break
        if not raw:
            return None
        msg = email.message_from_bytes(raw)
        body_text = _extract_text_body(msg)
        return {
            "uid": uid,
            "subject": _decode_header(msg.get("Subject")) or "(no subject)",
            "from": _from_addr(msg.get("From")),
            "date": _decode_header(msg.get("Date")) or "",
            "body": body_text,
        }
    except (imaplib.IMAP4.error, OSError) as exc:
        log.warning("IMAP fetch uid %s failed: %s", uid, exc)
        return None
    finally:
        try:
            conn.logout()
        except Exception:
            pass


def _extract_text_body(msg: email.message.Message) -> str:
    """Pull the first text/plain part out of ``msg``."""
    if msg.is_multipart():
        for part in msg.walk():
            if (
                part.get_content_type() == "text/plain"
                and "attachment" not in str(part.get("Content-Disposition") or "").lower()
            ):
                try:
                    payload = part.get_payload(decode=True) or b""
                    charset = part.get_content_charset() or "utf-8"
                    return payload.decode(charset, errors="replace")
                except (LookupError, TypeError):
                    continue
        return "(no text body)"
    try:
        payload = msg.get_payload(decode=True) or b""
        charset = msg.get_content_charset() or "utf-8"
        return payload.decode(charset, errors="replace")
    except (LookupError, TypeError):
        return "(unreadable body)"


def _smtp_send(to_addr: str, subject: str, body: str) -> tuple[bool, str]:
    """Send a single message. Returns ``(ok, human_message)``."""
    if not _smtp_configured():
        return False, "SMTP not configured (set SMTP_HOST/USER/PASS)."
    host = _env("SMTP_HOST") or ""
    port = int(_env("SMTP_PORT") or "587")
    user = _env("SMTP_USER") or ""
    pwd = _env("SMTP_PASS") or ""

    msg = EmailMessage()
    msg["From"] = user
    msg["To"] = to_addr
    msg["Subject"] = subject
    msg.set_content(body or "")

    try:
        ctx = ssl.create_default_context()
        with smtplib.SMTP(host, port, timeout=SMTP_TIMEOUT) as smtp:
            smtp.ehlo()
            try:
                smtp.starttls(context=ctx)
                smtp.ehlo()
            except smtplib.SMTPException:
                # Server may not support STARTTLS — try as-is.
                pass
            smtp.login(user, pwd)
            smtp.send_message(msg)
    except (smtplib.SMTPException, OSError, ValueError) as exc:
        log.warning("SMTP send failed: %s", exc)
        return False, f"Couldn't send: {exc}"
    return True, "Sent."


# ---- App ------------------------------------------------------------------ #


app = App(name="email")


@app.screen("/")
def inbox(_request=None):
    rows = _fetch_inbox()
    body: list[Any] = [Text("Inbox", style="heading")]
    if not rows:
        body.append(Text("No messages.", style="dim"))
    else:
        body.append(
            List(
                items=[
                    ListItem(
                        label=_trim(r.get("subject") or "(no subject)", SUBJECT_TRIM),
                        sub=f"{_trim(r.get('from') or '', 40)} · {_date_short(r.get('date'))}",
                        href=f"/msg?uid={r['uid']}",
                    )
                    for r in rows
                ]
            )
        )
    if not _imap_configured():
        body.append(Text("(mock inbox — set IMAP_* env vars)", style="dim"))
    body.append(Button(label="Refresh", href="/", method="GET"))
    body.append(Button(label="Compose", href="/compose", method="GET"))
    return Screen(id="email_inbox", title="Inbox", body=body, ttl=0)


@app.screen("/msg")
def message(request):
    uid = _first_q(request, "uid")
    if not uid:
        return Screen(
            id="email_msg_404",
            title="Email",
            body=[
                Notification(s="Missing uid.", level="warn"),
                Button(label="Inbox", href="/", method="GET"),
            ],
            ttl=0,
        )
    msg = _fetch_message(uid)
    if msg is None:
        return Screen(
            id="email_msg_404",
            title="Email",
            body=[
                Notification(s="Message not found.", level="warn"),
                Button(label="Inbox", href="/", method="GET"),
            ],
            ttl=0,
        )
    subject = msg.get("subject") or "(no subject)"
    body_preview = (msg.get("body") or "")[:BODY_PREVIEW]
    re_subject = subject if subject.lower().startswith("re:") else f"Re: {subject}"
    from_addr = msg.get("from") or ""
    return Screen(
        id="email_msg",
        title=_trim(subject, 40),
        body=[
            Text(_trim(subject, 80), style="heading"),
            Text(
                f"From: {from_addr}\nDate: {msg.get('date') or ''}",
                style="dim",
            ),
            Text(body_preview or "(empty body)", style="mono"),
            Button(
                label="Reply",
                href=f"/compose?to={from_addr}&subject={re_subject}",
                method="GET",
            ),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=0,
    )


@app.screen("/compose")
def compose_form(request):
    to_pref = _first_q(request, "to")
    subj_pref = _first_q(request, "subject")
    return Screen(
        id="email_compose",
        title="Compose",
        body=[
            Form(
                action="/compose",
                method="POST",
                submit="Send",
                fields=[
                    Input(name="to", label="To", value=to_pref or None),
                    Input(name="subject", label="Subject", value=subj_pref or None),
                    Input(name="body", label="Body", max=500),
                ],
            ),
            Button(label="Cancel", href="/", method="GET"),
        ],
        ttl=0,
    )


@app.handler("/compose", method="POST")
def compose_send(request):
    payload = request.body if isinstance(request.body, dict) else {}
    to_addr = (payload.get("to") or "").strip()
    subject = (payload.get("subject") or "").strip() or "(no subject)"
    body_text = (payload.get("body") or "").strip()

    if not to_addr:
        return Screen(
            id="email_compose_err",
            title="Compose",
            body=[
                Notification(s="Recipient is required.", level="error"),
                Button(label="Back", href="/compose", method="GET"),
                Button(label="Inbox", href="/", method="GET"),
            ],
            ttl=0,
        )

    ok, message_text = _smtp_send(to_addr, subject, body_text)
    return Screen(
        id="email_sent" if ok else "email_send_err",
        title="Email",
        body=[
            Notification(s=message_text, level="info" if ok else "error"),
            Button(label="Inbox", href="/", method="GET"),
        ],
        ttl=0,
    )


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
