"""Multi-instance fake-mesh tests (SIM-007 acceptance).

Spins up the MeshHub in-process, opens N TCP client sockets (each
playing the role of a simulator window), and verifies that packets
sent from one client reach every OTHER client and not the sender.
"""

from __future__ import annotations

import socket
import struct
import threading
import time

import pytest

from mesh_hub import LENGTH_HEADER, MeshHub, _serve_in_thread


def _connect(addr: tuple[str, int]) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(addr)
    s.settimeout(2.0)
    return s


def _send(s: socket.socket, packet: bytes) -> None:
    s.sendall(LENGTH_HEADER.pack(len(packet)) + packet)


def _recv(s: socket.socket, n: int = 1) -> list[bytes]:
    """Read exactly `n` length-prefixed packets."""
    out: list[bytes] = []
    buf = bytearray()
    while len(out) < n:
        chunk = s.recv(4096)
        if not chunk:
            raise EOFError("socket closed before n packets received")
        buf.extend(chunk)
        while True:
            if len(buf) < LENGTH_HEADER.size:
                break
            (length,) = LENGTH_HEADER.unpack_from(buf, 0)
            if len(buf) < LENGTH_HEADER.size + length:
                break
            out.append(bytes(buf[LENGTH_HEADER.size : LENGTH_HEADER.size + length]))
            del buf[: LENGTH_HEADER.size + length]
    return out


@pytest.fixture()
def hub():
    h = MeshHub(host="127.0.0.1", port=0)
    t = _serve_in_thread(h)
    yield h
    h.stop()
    t.join(timeout=1.0)


def _wait_until(predicate, timeout: float = 1.0, step: float = 0.01) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(step)
    raise AssertionError("timed out waiting for condition")


def test_three_clients_broadcast(hub: MeshHub):
    """A=B=C connected; A sends, B and C receive, A doesn't echo back."""
    a = _connect(hub.address)
    b = _connect(hub.address)
    c = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 3)

    _send(a, b"hello-from-A")

    assert _recv(b, 1) == [b"hello-from-A"]
    assert _recv(c, 1) == [b"hello-from-A"]

    # A should NOT see its own packet — verify by setting a short
    # timeout and asserting we get nothing.
    a.settimeout(0.2)
    with pytest.raises(socket.timeout):
        a.recv(4096)
    a.close(); b.close(); c.close()


def test_bidirectional_chatter(hub: MeshHub):
    """Two clients exchange messages; each only receives the other's."""
    a = _connect(hub.address)
    b = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 2)

    _send(a, b"from-A")
    _send(b, b"from-B")
    assert _recv(b, 1) == [b"from-A"]
    assert _recv(a, 1) == [b"from-B"]
    a.close(); b.close()


def test_oversized_packet_drops_sender(hub: MeshHub):
    """Sending a packet > MAX_PACKET disconnects the offender, others survive."""
    a = _connect(hub.address)
    b = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 2)

    big = b"X" * 9999
    a.sendall(struct.pack(">I", len(big)) + big)
    # Hub drops a; b should still work.
    _wait_until(lambda: hub.client_count == 1, timeout=1.5)
    c = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 2)
    _send(c, b"ok")
    assert _recv(b, 1) == [b"ok"]
    b.close(); c.close()
    try:
        a.close()
    except OSError:
        pass


def test_disconnect_recovers(hub: MeshHub):
    """Dropping one client doesn't kill the rest."""
    a = _connect(hub.address)
    b = _connect(hub.address)
    c = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 3)
    b.close()
    _wait_until(lambda: hub.client_count == 2, timeout=1.5)

    _send(a, b"after-b-left")
    assert _recv(c, 1) == [b"after-b-left"]
    a.close(); c.close()


def test_stats_counter(hub: MeshHub):
    """packets_forwarded counts deliveries (sender excluded)."""
    a = _connect(hub.address)
    b = _connect(hub.address)
    c = _connect(hub.address)
    _wait_until(lambda: hub.client_count == 3)
    _send(a, b"one")
    _recv(b, 1); _recv(c, 1)
    _send(a, b"two")
    _recv(b, 1); _recv(c, 1)
    # 2 packets × 2 recipients each = 4 deliveries
    _wait_until(lambda: hub.packets_forwarded == 4)
    a.close(); b.close(); c.close()
