#!/usr/bin/env python3
"""Fake LoRa mesh hub for multi-instance simulator runs (SIM-007).

Run this once; then launch N simulator instances each configured with
``--proxy-mesh-hub=tcp://127.0.0.1:<port>`` (or set
``PAGEROS_MESH_HUB``). The hub broadcasts every LoRa packet it receives
to all OTHER connected clients, so two simulators running the chat
example can exchange group messages without a real LoRa radio.

Wire format on the socket:

    [u32 length-prefix big-endian][raw LoRa envelope bytes]

The hub doesn't decode envelopes — it just forwards bytes. That keeps
the hub honest (it has no idea what apps are talking about) and lets
us reuse the same plumbing for non-LoRa simulator features later.

Why TCP and not the simulator-internal Tauri channel: the hub must work
across separate processes (one per simulator window), and TCP is the
lowest-friction cross-platform option that doesn't require a Tauri
plugin.

Tauri integration is tracked separately; the simulator's
``proxy.rs`` (SIM-006) is the natural insertion point — it already
forwards "simulated LoRa" packets and just needs to also CC them to the
hub. See ``README.md`` § "Multi-instance group testing" for the wiring
plan.
"""

from __future__ import annotations

import argparse
import logging
import selectors
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Iterable

LENGTH_HEADER = struct.Struct(">I")  # u32 big-endian
MAX_PACKET = 256  # SPEC §6.2.1: LoRa MTU


@dataclass
class _ClientState:
    sock: socket.socket
    addr: tuple
    buf: bytearray = field(default_factory=bytearray)
    expect: int | None = None  # None when waiting for the length header


class MeshHub:
    """A TCP server that broadcasts every received packet to all other clients.

    The hub is self-contained and embeddable: tests instantiate it with a
    random port and drive it programmatically; the CLI just wraps that
    same object.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 0) -> None:
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.bind((host, port))
        self._listen.listen(64)
        self._listen.setblocking(False)
        self._sel = selectors.DefaultSelector()
        self._sel.register(self._listen, selectors.EVENT_READ)
        self._clients: dict[socket.socket, _ClientState] = {}
        self._stop = threading.Event()
        self._stats_lock = threading.Lock()
        self._packets_forwarded = 0

    @property
    def address(self) -> tuple[str, int]:
        return self._listen.getsockname()

    @property
    def packets_forwarded(self) -> int:
        with self._stats_lock:
            return self._packets_forwarded

    @property
    def client_count(self) -> int:
        return len(self._clients)

    def stop(self) -> None:
        self._stop.set()
        try:
            self._listen.close()
        except OSError:
            pass

    def serve_forever(self, *, idle_tick: float = 0.5) -> None:
        log = logging.getLogger("pageros.mesh-hub")
        log.info("listening on %s:%d", *self.address)
        while not self._stop.is_set():
            for key, _ in self._sel.select(timeout=idle_tick):
                fileobj = key.fileobj
                if fileobj is self._listen:
                    self._accept()
                else:
                    self._read(fileobj)
        # Best-effort cleanup
        for cs in list(self._clients.values()):
            self._drop(cs.sock)

    # --- internals --------------------------------------------------- #

    def _accept(self) -> None:
        try:
            conn, addr = self._listen.accept()
        except OSError:
            return
        conn.setblocking(False)
        self._sel.register(conn, selectors.EVENT_READ)
        self._clients[conn] = _ClientState(sock=conn, addr=addr)
        logging.getLogger("pageros.mesh-hub").info("client connected: %s", addr)

    def _read(self, sock: socket.socket) -> None:
        cs = self._clients.get(sock)
        if cs is None:
            return
        try:
            chunk = sock.recv(4096)
        except OSError:
            self._drop(sock)
            return
        if not chunk:
            self._drop(sock)
            return
        cs.buf.extend(chunk)
        self._drain(cs)

    def _drain(self, cs: _ClientState) -> None:
        while True:
            if cs.expect is None:
                if len(cs.buf) < LENGTH_HEADER.size:
                    return
                (length,) = LENGTH_HEADER.unpack_from(cs.buf, 0)
                del cs.buf[: LENGTH_HEADER.size]
                if length > MAX_PACKET:
                    logging.getLogger("pageros.mesh-hub").warning(
                        "client %s sent oversized packet %d; dropping", cs.addr, length
                    )
                    self._drop(cs.sock)
                    return
                cs.expect = length
            if cs.expect is not None and len(cs.buf) < cs.expect:
                return
            assert cs.expect is not None  # for type checkers
            pkt = bytes(cs.buf[: cs.expect])
            del cs.buf[: cs.expect]
            cs.expect = None
            self._broadcast(sender=cs.sock, packet=pkt)

    def _broadcast(self, *, sender: socket.socket, packet: bytes) -> None:
        framed = LENGTH_HEADER.pack(len(packet)) + packet
        delivered = 0
        for sock in list(self._clients):
            if sock is sender:
                continue
            try:
                sock.sendall(framed)
                delivered += 1
            except OSError:
                self._drop(sock)
        with self._stats_lock:
            self._packets_forwarded += delivered

    def _drop(self, sock: socket.socket) -> None:
        cs = self._clients.pop(sock, None)
        try:
            self._sel.unregister(sock)
        except (KeyError, ValueError):
            pass
        try:
            sock.close()
        except OSError:
            pass
        if cs is not None:
            logging.getLogger("pageros.mesh-hub").info("client disconnected: %s", cs.addr)


def _serve_in_thread(hub: MeshHub) -> threading.Thread:
    t = threading.Thread(target=hub.serve_forever, daemon=True)
    t.start()
    return t


def _cli(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="PagerOS fake LoRa mesh hub (SIM-007).")
    p.add_argument("--host", default="127.0.0.1", help="bind host (default 127.0.0.1)")
    p.add_argument("--port", type=int, default=49000, help="bind port (default 49000)")
    p.add_argument("--verbose", "-v", action="store_true")
    args = p.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    hub = MeshHub(host=args.host, port=args.port)
    try:
        hub.serve_forever()
    except KeyboardInterrupt:
        hub.stop()
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
