# pageros (Python SDK)

Python SDK for building PagerOS apps. See repo root `SPEC.md` and `TASKS.md`.

## Dev server (PY-009)

Run a user app with auto-reload on file change:

```
python -m pageros.devserver --app path/to/app.py --host 127.0.0.1 --port 8000
```

`pagerctl dev` (CLI-002) wraps this entry point and pairs it with the simulator.

## LoRa size budget (PY-011)

Apps that set `lora_compatible=True` must keep encoded Frames at or below
200 bytes so they fit in a single LoRa packet. The SDK exposes a helper
that logs a warning when a Frame exceeds that budget:

```python
from pageros import check_frame_size

check_frame_size(encoded_frame, lora_compatible=app.lora_compatible)
```

`encoded_frame` may be the CBOR bytes (any bytes-like) or its length as an
`int`. Pass `frame_label="GET /home"` to include the route in the warning.

