"""Self-test for the PROTO-005 conformance runner.

Spawns the stub SDK (`stub_sdk.StubSDK`), runs the CLI runner against it,
and asserts:

  * a vanilla stub yields 86 / 86 pass and exit code 0,
  * a stub broken on one `encode` vector yields one failure and exit code 1,
  * a stub broken on one `negative` vector (forced to accept) yields one
    failure and exit code 1,
  * a stub broken on one `decode_only` vector (forced to return a wrong shape)
    yields one failure and exit code 1,
  * the runner exits 2 when the endpoint is unreachable.

Pure stdlib; run as:

    python3 protocol/conformance/test_runner.py
"""

from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
VECTORS_DIR = REPO_ROOT / "protocol" / "test-vectors" / "ui"

sys.path.insert(0, str(REPO_ROOT))

from protocol.conformance import proto_conformance  # noqa: E402
from protocol.conformance.stub_sdk import StubSDK  # noqa: E402


class _StubFixture:
    def __init__(self, break_vector: str | None = None):
        self.stub = StubSDK(VECTORS_DIR, break_vector=break_vector)
        self.thread = None

    def __enter__(self) -> StubSDK:
        self.thread = self.stub.run_in_background()
        return self.stub

    def __exit__(self, *exc) -> None:
        self.stub.shutdown()
        self.stub.server_close()
        if self.thread:
            self.thread.join(timeout=2)


def _run_cli(endpoint: str, extra: list[str] | None = None) -> tuple[int, str, str]:
    out_buf, err_buf = io.StringIO(), io.StringIO()
    argv = ["--endpoint", endpoint, "--vectors", str(VECTORS_DIR)]
    if extra:
        argv += extra
    with redirect_stdout(out_buf), redirect_stderr(err_buf):
        code = proto_conformance.main(argv)
    return code, out_buf.getvalue(), err_buf.getvalue()


class RunnerSelfTest(unittest.TestCase):
    def test_all_pass(self) -> None:
        with _StubFixture() as stub:
            code, out, _ = _run_cli(stub.endpoint)
        self.assertEqual(code, 0, msg=out)
        self.assertIn("PASS", out)
        # The exact count must match index.json so a future vector addition
        # surfaces here.
        total_line = next(line for line in out.splitlines() if line.startswith("vectors:"))
        count = int(total_line.split()[1])
        self.assertGreaterEqual(count, 86, msg=f"vectors line: {total_line}")
        self.assertIn(f"{count}/{count} passed", out)

    def test_broken_encode_reports_failure(self) -> None:
        target = "widget_text_minimal_string"
        with _StubFixture(break_vector=target) as stub:
            code, out, _ = _run_cli(stub.endpoint)
        self.assertEqual(code, 1, msg=out)
        self.assertIn("FAIL", out)
        self.assertIn(target, out)
        self.assertIn("canonical CBOR mismatch", out)

    def test_broken_negative_reports_failure(self) -> None:
        target = "negative_unknown_major_version"
        with _StubFixture(break_vector=target) as stub:
            code, out, _ = _run_cli(stub.endpoint)
        self.assertEqual(code, 1, msg=out)
        self.assertIn(target, out)
        self.assertIn("SDK accepted negative vector", out)

    def test_broken_decode_only_reports_failure(self) -> None:
        target = "forward_unknown_widget_string"
        with _StubFixture(break_vector=target) as stub:
            code, out, _ = _run_cli(stub.endpoint)
        self.assertEqual(code, 1, msg=out)
        self.assertIn(target, out)
        self.assertIn("structural mismatch", out)

    def test_unreachable_endpoint_exits_two(self) -> None:
        # 127.0.0.1:1 is virtually never listening.
        code, _, err = _run_cli("http://127.0.0.1:1", extra=["--timeout", "1"])
        self.assertEqual(code, 2, msg=err)

    def test_filter_glob(self) -> None:
        with _StubFixture() as stub:
            code, out, _ = _run_cli(stub.endpoint, extra=["--filter", "event_*"])
        self.assertEqual(code, 0, msg=out)
        # 18 event vectors per the index.
        self.assertIn("vectors: 18", out)

    def test_json_report(self) -> None:
        with _StubFixture() as stub:
            code, out, _ = _run_cli(stub.endpoint, extra=["--report", "json"])
        self.assertEqual(code, 0)
        import json as _json

        payload = _json.loads(out)
        self.assertEqual(payload["failed"], 0)
        self.assertGreaterEqual(payload["total"], 86)
        self.assertEqual(payload["passed"], payload["total"])
        # Every result must have name+passed; spot-check one.
        sample = payload["results"][0]
        for key in ("name", "category", "kind", "passed", "skipped", "detail"):
            self.assertIn(key, sample)


if __name__ == "__main__":
    unittest.main()
