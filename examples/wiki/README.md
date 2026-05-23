# Wiki — Wikipedia on your pager

`wiki.pageros.app` is a small PagerOS app that lets you search Wikipedia
and read article summaries on the 480x222 LILYGO T-LoRa pager. Extracts
are trimmed to roughly 1200 characters so each article fits a few
screens of scroll.

## Screens

- `/` — search box + "Surprise me" random-article button.
- `/search?q=Q` — top 10 OpenSearch results.
- `/article?t=TITLE` — page summary (title, description, extract).
- `/random` — random article summary.

## Run

```sh
pip install -r requirements.txt
python app.py --host 0.0.0.0 --port 8019
```

Then sideload `http://<your-host>:8019/` from the PagerOS device.

The app only makes outbound calls to `en.wikipedia.org` and sends the
`User-Agent: pageros-wiki/1.0 (demo@pageros.test)` header per
Wikipedia's API etiquette. No API key required.
