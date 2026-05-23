# `stocks` — live watchlist for stocks & crypto

A small per-device watchlist that pulls live prices from Yahoo Finance
(stocks) and CoinGecko (crypto), with a 60-second in-process cache so
the screen renders quickly without hammering the upstream APIs.

The default device starts seeded with `AAPL`, `GOOGL`, `MSFT`, `BTC`,
and `ETH`. Each pager that signs requests with a different X25519
pubkey (`ctx.device_id`) gets its own list.

## Routes

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Watchlist with live prices, plus Add / Refresh. |
| `GET` | `/add` | Form to add a new symbol + kind. |
| `POST` | `/add` | Append to the per-device watchlist; back to `/`. |
| `GET` | `/ticker?s=SYM` | Detail screen: price, day change, Remove + Back. |
| `POST` | `/remove?s=SYM` | Remove from the watchlist (confirmed). |

## Storage

`watchlist.json` next to `app.py` (override with
`PAGEROS_STOCKS_STORE`). Maps device id → list of
`{symbol, kind}` rows; `kind` is `"stock"` or `"crypto"`.

## Run it

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python app.py --host 127.0.0.1 --port 8018
```

Open in the simulator at `http://127.0.0.1:8018/`, or sideload via
**Settings → Apps → Add by URL** on a real pager.
