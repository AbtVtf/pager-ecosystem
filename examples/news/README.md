# News — Hacker News for PagerOS

A two-screen PagerOS app that shows the top Hacker News headlines and
lets you drill into a single story.

- `/` — heading "Hacker News" plus the top 15 stories
  (`title`, `score`, comment count, domain).
- `/story?id=<id>` — story title, points/comments, domain, and an
  "Open external" button pointing at the upstream URL.

Stories are fetched from the public Firebase HN API
(`https://hacker-news.firebaseio.com/v0/`) and cached in process for
5 minutes. If the network is down, the app falls back to a small
hardcoded story list so the screens still render.

## Run it

```bash
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8011
```

Then point the pager (or `pageros-cli`) at `http://<host>:8011/`.
