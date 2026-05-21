# Simulator tools

## `mesh_hub.py` — fake LoRa mesh for multi-instance runs (SIM-007)

Run this once; then launch N simulator instances each configured to
connect to it. Every LoRa-sized packet a simulator sends is broadcast
to every OTHER connected simulator — that's the "fake mesh" that lets
two simulator windows running the chat example exchange group messages
without a real LoRa radio.

### Running

```sh
# 1) Start the hub (foreground; Ctrl-C to stop).
python3 simulator/tools/mesh_hub.py --port 49000

# 2) In another terminal — launch simulator A.
PAGEROS_MESH_HUB=tcp://127.0.0.1:49000 \
  PAGEROS_SIMULATOR_PROFILE=alice \
  pageros-simulator --connect http://localhost:8080

# 3) In a third terminal — launch simulator B against the same chat app.
PAGEROS_MESH_HUB=tcp://127.0.0.1:49000 \
  PAGEROS_SIMULATOR_PROFILE=bob \
  pageros-simulator --connect http://localhost:8080
```

Two simulator windows show up; both joined the same chat room exchange
messages live through the chat app server (HTTP path) and would
exchange them through the hub when offline.

### Wire format (TCP)

The hub doesn't decode envelopes — it just length-prefixes and forwards
bytes. That keeps the hub honest (no app knowledge) and lets us reuse
the same plumbing for non-LoRa simulator features later.

```
┌──────────────────────┬───────────────────────────┐
│ length (u32 BE)      │ raw LoRa envelope bytes  │
└──────────────────────┴───────────────────────────┘
```

Packets > 256 B (SPEC §6.2.1 LoRa MTU) cause the sender to be dropped.

### Status of the simulator-side wiring

The Tauri simulator's `proxy.rs` (SIM-006) is the natural insertion
point — it already simulates LoRa packets with injectable latency/loss;
the addition is to CC every packet to the hub and consume incoming
packets from it. That wiring lives in a follow-up; the hub itself is
the load-bearing piece this task delivers and is fully tested in
`test_mesh_hub.py` against the multi-process scenarios the spec calls
out (3+ instances, sender doesn't see its own packet, disconnect/reconnect
resilience, oversized-packet defence).

### Tests

```sh
cd simulator/tools && python3 -m pytest test_mesh_hub.py -v
```

5 tests, all pass; they cover:

- 3-client broadcast (A→{B,C}; A doesn't echo back)
- Bidirectional 2-client chatter
- Oversized-packet sender gets dropped, others survive
- Disconnect of one client doesn't kill the rest
- `packets_forwarded` accounting

### Why Python and not Rust

The hub is a 50-line socket server; a Python script is the lowest-
friction artifact to ship + run alongside the simulator. Future plan:
either fold the hub into the Tauri simulator as a leader-mode option
(first instance hosts; others connect), or rewrite in Rust and ship as
`pageros-mesh-hub`.
