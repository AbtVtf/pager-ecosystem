# email — PagerOS demo app

IMAP inbox viewer with SMTP reply, sized for the LILYGO T-LoRa pager
(480x222). Built on `pageros` SDK; uses only stdlib `imaplib` / `smtplib`
for network.

## Screens

- `/` — heading "Inbox" + 20 most-recent messages + Refresh / Compose.
- `/msg?uid=<n>` — subject + sender + date + first 800 chars + Reply / Back.
- `/compose` — Form (to / subject / body) pre-filled from `?to=` and
  `?subject=` query params. POSTs back to `/compose` to send via SMTP.

If `IMAP_HOST` / `IMAP_USER` / `IMAP_PASS` are missing, or the IMAP
server is unreachable, the inbox falls back to 5 mock messages so the
UI still renders. SMTP failures surface as an in-screen
`Notification` rather than a 500.

## Environment variables

| Var         | Default | Notes                       |
|-------------|---------|-----------------------------|
| `IMAP_HOST` | —       | Required for real inbox     |
| `IMAP_USER` | —       | Required for real inbox     |
| `IMAP_PASS` | —       | Required for real inbox     |
| `IMAP_PORT` | `993`   | SSL                         |
| `SMTP_HOST` | —       | Required for sending mail   |
| `SMTP_USER` | —       | Used as `From:`             |
| `SMTP_PASS` | —       |                             |
| `SMTP_PORT` | `587`   | STARTTLS attempted, optional|

## Run

```sh
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8012
```

No env vars? You still get a working mock inbox at <http://127.0.0.1:8012/>.
