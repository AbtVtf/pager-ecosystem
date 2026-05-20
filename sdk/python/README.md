# pageros (Python SDK)

Python SDK for building PagerOS apps. See repo root `SPEC.md` and `TASKS.md`.

## Dev server (PY-009)

Run a user app with auto-reload on file change:

```
python -m pageros.devserver --app path/to/app.py --host 127.0.0.1 --port 8000
```

`pagerctl dev` (CLI-002) wraps this entry point and pairs it with the simulator.
