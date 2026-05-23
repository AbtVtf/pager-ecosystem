# SSH

A tiny PagerOS marketplace example: a one-shot SSH client for the LILYGO
T-LoRa pager. Save a few hosts, pick one, type a command, see the output.

## What it does

- **Home (`/`)** — list of saved hosts with `user@host:port` subtitles, plus
  an "Add host" button.
- **Add (`/add`)** — form to save a new host (nickname, host, port, user,
  optional password, optional private-key path).
- **Host (`/host?n=NICK`)** — a command input that POSTs to `/run`, plus a
  "Delete host" button (with confirm).
- **Run (`/run?n=NICK`)** — connects via paramiko, runs the command with a
  10s exec timeout, and renders stdout (first 2000 chars) in `mono` style,
  with stderr rendered dim underneath.

Hosts persist to `ssh_hosts.json` next to `app.py`.

## Security note

This example is a **local dev demo**. It does two things you do **not**
want in production:

1. It uses `paramiko.AutoAddPolicy()` — unknown host keys are silently
   accepted on first connect. A real client must verify host fingerprints.
2. It stores passwords in plaintext inside `ssh_hosts.json`. Prefer
   key-based auth (`key_path`) and treat the JSON file as sensitive.

Run it only against hosts you control, and don't commit
`ssh_hosts.json`.

## How to run

```bash
pip install -r requirements.txt
python app.py --port 8016
```

Then point the pager (or the dev simulator) at
`http://127.0.0.1:8016/`.
