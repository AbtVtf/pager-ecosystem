# Drive — cloud file storage for PagerOS

A tiny file-browser app backed by a local directory on the host. Lets
you walk folders, peek at small text files inline, create folders,
upload short text uploads, and delete files / empty folders.

- `/` and `/browse?p=PATH` — folder listing (folders first, then files
  with size + mtime).
- `/view?p=PATH` — file detail; inline preview when the file is text
  and at most 4 KB, otherwise a "Binary file" placeholder.
- `/upload?p=PATH` — text-content upload form (`filename` + `content`,
  refuses overwrites).
- `/mkdir?p=PATH` — create a new folder.
- `/delete?p=PATH` (POST) — delete a file or empty folder.

Storage lives in `./drive_store/` next to `app.py` (created on first
run). Override the location with the `DRIVE_ROOT` environment
variable. Every filesystem operation routes through a `safe_path`
helper that refuses any request escaping the root.

## Run it

```bash
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8015
```

Then point the pager (or `pageros-cli`) at `http://<host>:8015/`.
