# Translate — LibreTranslate for PagerOS

A small PagerOS app that translates short text between 12 languages
(en, es, fr, de, it, pt, nl, ja, zh, hi, ar, ru) using a
[LibreTranslate](https://libretranslate.com)-compatible endpoint, and
keeps the last 10 translations per device.

## Screens

- `/` — translate form (text + source + target).
- `/do` (POST) — translation result.
- `/history` — recent translations for this device, with clear/back.

## Environment

- `LIBRETRANSLATE_URL` — base URL of the LibreTranslate service.
  Defaults to `https://libretranslate.com`. Point at a self-hosted
  instance to avoid the public rate limit, e.g.
  `http://localhost:5000`.
- `LIBRETRANSLATE_API_KEY` — optional API key forwarded as `api_key`
  on each translate call.

If the upstream is unreachable the app falls back to a `[stub] <text>`
response so the screens keep rendering during demos.

## Run it

```bash
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8020
```

Then point the pager (or `pageros-cli`) at `http://<host>:8020/`.
