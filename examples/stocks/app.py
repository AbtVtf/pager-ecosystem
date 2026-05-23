"""PagerOS stocks & crypto watchlist example.

A per-device watchlist of stock tickers and cryptocurrencies with live
prices and day-over-day change. Demonstrates:

- ``list`` + ``list_item`` rows linking through to a detail screen,
- ``form`` + ``input`` for adding new tickers,
- ``button`` with ``confirm`` for delete,
- ``ctx.device_id`` for per-pager watchlists (SPEC §8.3),
- A short in-process price cache so we don't hammer Yahoo / CoinGecko
  on every screen refresh.

Per-device watchlist is persisted to ``watchlist.json`` next to
``app.py`` — the same JSON-blob pattern as the ``notes`` example.

Routes
------

| Method | Path | Purpose |
| --- | --- | --- |
| ``GET`` | ``/`` | Watchlist with live prices + Add / Refresh buttons. |
| ``GET`` | ``/add`` | Form to add a new symbol. |
| ``POST`` | ``/add`` | Append to the per-device watchlist. |
| ``GET`` | ``/ticker?s=SYM`` | Detail: price, change, Remove + Back. |
| ``POST`` | ``/remove?s=SYM`` | Remove from the watchlist (confirmed). |
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

import requests

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

log = logging.getLogger("examples.stocks")


# --------------------------------------------------------------------------- #
# Storage
# --------------------------------------------------------------------------- #

DEFAULT_STORE = Path(__file__).with_name("watchlist.json")
STORE_PATH = Path(os.environ.get("PAGEROS_STOCKS_STORE", str(DEFAULT_STORE)))

# Default device key (used for anonymous dev requests, matching the
# notes example convention).
DEFAULT_DEVICE = ""

# Seed list installed for the default device on first run.
SEED_WATCHLIST: list[dict[str, str]] = [
    {"symbol": "AAPL", "kind": "stock"},
    {"symbol": "GOOGL", "kind": "stock"},
    {"symbol": "MSFT", "kind": "stock"},
    {"symbol": "BTC", "kind": "crypto"},
    {"symbol": "ETH", "kind": "crypto"},
]

# Symbol → CoinGecko id. Small curated map; unmapped crypto symbols
# fall back to a lowercased id (works for most majors but not all).
COINGECKO_IDS: dict[str, str] = {
    "BTC": "bitcoin",
    "ETH": "ethereum",
    "SOL": "solana",
    "DOGE": "dogecoin",
    "ADA": "cardano",
    "XRP": "ripple",
}

HTTP_TIMEOUT = 5.0
USER_AGENT = "Mozilla/5.0 pageros-stocks"


class WatchlistStore:
    """JSON-backed per-device watchlist."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._lock = threading.Lock()
        self._data: dict[str, list[dict[str, str]]] = self._load()
        # Seed the default device if this is a brand-new store.
        if DEFAULT_DEVICE not in self._data:
            with self._lock:
                self._data[DEFAULT_DEVICE] = [dict(t) for t in SEED_WATCHLIST]
                self._flush_unlocked()

    def _load(self) -> dict[str, list[dict[str, str]]]:
        if not self.path.exists():
            return {}
        try:
            raw = json.loads(self.path.read_text("utf-8"))
        except (OSError, ValueError):
            log.warning("watchlist store %s is malformed; starting fresh", self.path)
            return {}
        if not isinstance(raw, dict):
            return {}
        return {k: list(v) for k, v in raw.items() if isinstance(v, list)}

    def _flush_unlocked(self) -> None:
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(
            json.dumps(self._data, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        os.replace(tmp, self.path)

    def list(self, device_id: str) -> list[dict[str, str]]:
        with self._lock:
            return [dict(t) for t in self._data.get(device_id, ())]

    def add(self, device_id: str, symbol: str, kind: str) -> bool:
        symbol = symbol.upper().strip()
        kind = kind.lower().strip()
        if kind not in ("stock", "crypto"):
            kind = "stock"
        if not symbol:
            return False
        with self._lock:
            tickers = self._data.setdefault(device_id, [])
            for t in tickers:
                if t.get("symbol") == symbol:
                    return False
            tickers.append({"symbol": symbol, "kind": kind})
            self._flush_unlocked()
            return True

    def remove(self, device_id: str, symbol: str) -> bool:
        symbol = symbol.upper().strip()
        with self._lock:
            tickers = self._data.get(device_id)
            if not tickers:
                return False
            remaining = [t for t in tickers if t.get("symbol") != symbol]
            if len(remaining) == len(tickers):
                return False
            self._data[device_id] = remaining
            self._flush_unlocked()
            return True

    def find(self, device_id: str, symbol: str) -> dict[str, str] | None:
        symbol = symbol.upper().strip()
        with self._lock:
            for t in self._data.get(device_id, ()):
                if t.get("symbol") == symbol:
                    return dict(t)
        return None


store = WatchlistStore(STORE_PATH)


# --------------------------------------------------------------------------- #
# Price fetch (with a 60 s in-process cache)
# --------------------------------------------------------------------------- #

CACHE_TTL = 60.0  # seconds
_price_cache: dict[str, tuple[float, tuple[float | None, float | None]]] = {}
_price_cache_lock = threading.Lock()


def _cache_get(key: str) -> tuple[float | None, float | None] | None:
    with _price_cache_lock:
        entry = _price_cache.get(key)
        if entry is None:
            return None
        ts, value = entry
        if time.monotonic() - ts > CACHE_TTL:
            return None
        return value


def _cache_put(key: str, value: tuple[float | None, float | None]) -> None:
    with _price_cache_lock:
        _price_cache[key] = (time.monotonic(), value)


def _fetch_stock(symbol: str) -> tuple[float | None, float | None]:
    """Return ``(price, pct_change)`` for a Yahoo-quoted ticker."""
    url = "https://query1.finance.yahoo.com/v7/finance/quote"
    try:
        resp = requests.get(
            url,
            params={"symbols": symbol},
            headers={"User-Agent": USER_AGENT},
            timeout=HTTP_TIMEOUT,
        )
        resp.raise_for_status()
        payload = resp.json()
        result = payload.get("quoteResponse", {}).get("result") or []
        if not result:
            return None, None
        row = result[0]
        price = row.get("regularMarketPrice")
        pct = row.get("regularMarketChangePercent")
        price_f = float(price) if price is not None else None
        pct_f = float(pct) if pct is not None else None
        return price_f, pct_f
    except (requests.RequestException, ValueError, KeyError, TypeError) as exc:
        log.warning("stock fetch failed for %s: %s", symbol, exc)
        return None, None


def _fetch_crypto(symbol: str) -> tuple[float | None, float | None]:
    """Return ``(price_usd, pct_24h_change)`` for a CoinGecko coin."""
    coin_id = COINGECKO_IDS.get(symbol.upper(), symbol.lower())
    url = "https://api.coingecko.com/api/v3/simple/price"
    try:
        resp = requests.get(
            url,
            params={
                "ids": coin_id,
                "vs_currencies": "usd",
                "include_24hr_change": "true",
            },
            headers={"User-Agent": USER_AGENT},
            timeout=HTTP_TIMEOUT,
        )
        resp.raise_for_status()
        payload = resp.json()
        row = payload.get(coin_id) or {}
        price = row.get("usd")
        pct = row.get("usd_24h_change")
        price_f = float(price) if price is not None else None
        pct_f = float(pct) if pct is not None else None
        return price_f, pct_f
    except (requests.RequestException, ValueError, KeyError, TypeError) as exc:
        log.warning("crypto fetch failed for %s: %s", symbol, exc)
        return None, None


def price_for(symbol: str, kind: str) -> tuple[float | None, float | None]:
    cache_key = f"{kind}:{symbol.upper()}"
    cached = _cache_get(cache_key)
    if cached is not None:
        return cached
    if kind == "crypto":
        value = _fetch_crypto(symbol)
    else:
        value = _fetch_stock(symbol)
    _cache_put(cache_key, value)
    return value


# --------------------------------------------------------------------------- #
# App
# --------------------------------------------------------------------------- #

app = App(name="stocks")


def _format_price(price: float | None) -> str:
    return f"${price:,.2f}" if price is not None else "$? n/a"


def _format_pct(pct: float | None) -> str:
    return f"{pct:+.2f}% today" if pct is not None else "n/a today"


def _query_symbol(request) -> str:
    values = request.query.get("s") or []
    return values[0].upper().strip() if values else ""


def _list_screen(device_id: str, *, flash: str | None = None) -> Screen:
    tickers = store.list(device_id)
    body: list[Any] = [Text("Watchlist", style="heading")]
    if flash:
        body.append(Notification(s=flash))
    if not tickers:
        body.append(Text("Empty — add a ticker to begin.", style="dim"))
    else:
        items: list[ListItem] = []
        for t in tickers:
            sym = t.get("symbol", "")
            kind = t.get("kind", "stock")
            price, pct = price_for(sym, kind)
            label = f"{sym}  {_format_price(price)}"
            sub = f"{_format_pct(pct)}  · {kind}"
            items.append(
                ListItem(label=label, sub=sub, href=f"/ticker?s={sym}")
            )
        body.append(List(items=items))
    body.append(Button(label="Add ticker", href="/add", method="GET"))
    body.append(Button(label="Refresh", href="/", method="GET"))
    return Screen(id="stocks_home", title="Stocks", body=body, ttl=0)


@app.screen("/")
def home(request):
    return _list_screen(request.ctx.device_id or DEFAULT_DEVICE)


@app.screen("/add")
def add_form(_request=None):
    return Screen(
        id="stocks_add",
        title="Add ticker",
        body=[
            Form(
                action="/add",
                method="POST",
                submit="Add",
                fields=[
                    Input(name="symbol", label="Symbol (AAPL or BTC)"),
                    Input(
                        name="kind",
                        label="stock or crypto",
                        value="stock",
                    ),
                ],
            )
        ],
        ttl=0,
    )


@app.handler("/add", method="POST")
def add_submit(request):
    device_id = request.ctx.device_id or DEFAULT_DEVICE
    body = request.body if isinstance(request.body, dict) else {}
    symbol = (body.get("symbol") or "").strip()
    kind = (body.get("kind") or "stock").strip().lower()
    if not symbol:
        return _list_screen(device_id, flash="Symbol required.")
    ok = store.add(device_id, symbol, kind)
    flash = f"Added {symbol.upper()}." if ok else f"{symbol.upper()} already on list."
    return _list_screen(device_id, flash=flash)


@app.screen("/ticker")
def ticker_detail(request):
    device_id = request.ctx.device_id or DEFAULT_DEVICE
    symbol = _query_symbol(request)
    ticker = store.find(device_id, symbol)
    if ticker is None:
        return Screen(
            id="stocks_missing",
            title="Stocks",
            body=[
                Notification(s="Not on your watchlist.", level="warn"),
                Button(label="Back", href="/", method="GET"),
            ],
            ttl=0,
        )
    kind = ticker.get("kind", "stock")
    price, pct = price_for(symbol, kind)
    return Screen(
        id="stocks_detail",
        title=symbol,
        body=[
            Text(symbol, style="heading"),
            Text(kind, style="dim"),
            Text(f"Price: {_format_price(price)}", style="body"),
            Text(f"Change: {_format_pct(pct)}", style="body"),
            Button(
                label="Remove",
                href=f"/remove?s={symbol}",
                method="POST",
                confirm=f"Remove {symbol}?",
            ),
            Button(label="Back", href="/", method="GET"),
        ],
        ttl=0,
    )


@app.handler("/remove", method="POST")
def remove_submit(request):
    device_id = request.ctx.device_id or DEFAULT_DEVICE
    symbol = _query_symbol(request)
    ok = store.remove(device_id, symbol)
    flash = f"Removed {symbol}." if ok else f"{symbol} not found."
    return _list_screen(device_id, flash=flash)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    app.run()
