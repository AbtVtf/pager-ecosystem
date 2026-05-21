// LoRa size budget warning (JS-011).
//
// Mirrors `sdk/python/pageros/lora_budget.py` — SPEC §1 and §8.4 give
// Frames a hard 200 B ceiling so they fit in one LoRa packet. Apps that
// opt into `lora_compatible: true` promise to stay under that ceiling;
// this helper inspects an already-encoded Frame and logs a single
// warning per oversized response. It does NOT throw — going over budget
// still works on Wi-Fi, and a misconfigured Frame shouldn't crash an
// app server.
//
// Wire this in at the Frame-encode boundary (next to your `encodeFrame`
// call) for the warning to fire when it matters.

/** SPEC §1 — single LoRa packet payload ceiling. */
export const LORA_FRAME_BUDGET_BYTES = 200;

export interface CheckFrameSizeOptions {
  /**
   * The app's manifest `lora_compatible` flag. The budget only applies
   * when this is true; otherwise the check is a no-op and returns false.
   */
  loraCompatible: boolean;

  /**
   * Optional label for the warning message — typically the route or
   * screen id (`"/notes"`, `"scr_home"`). When omitted the warning just
   * names the size.
   */
  frameLabel?: string;

  /**
   * Sink for the warning. Defaults to `console.warn`. Tests inject a
   * spy so assertions are deterministic.
   */
  logger?: (message: string) => void;
}

/**
 * Warn if `encoded` exceeds the LoRa Frame budget.
 *
 * `encoded` is either the CBOR-encoded Frame bytes or its length. The
 * length form is provided so callers that already know the size don't
 * have to re-measure.
 *
 * Returns `true` if a warning was logged, `false` otherwise.
 */
export function checkFrameSize(
  encoded: Uint8Array | ArrayBufferView | number,
  options: CheckFrameSizeOptions,
): boolean {
  if (!options.loraCompatible) return false;

  let size: number;
  if (typeof encoded === "number") {
    if (!Number.isFinite(encoded) || encoded < 0) {
      throw new RangeError("encoded size must be a non-negative number");
    }
    size = encoded;
  } else {
    size = encoded.byteLength;
  }

  if (size <= LORA_FRAME_BUDGET_BYTES) return false;

  const log = options.logger ?? ((m: string) => console.warn(m));
  const overage = size - LORA_FRAME_BUDGET_BYTES;
  const label = options.frameLabel ? ` (${options.frameLabel})` : "";
  log(
    `pageros: encoded Frame${label} is ${size} bytes, exceeds the LoRa ` +
      `budget of ${LORA_FRAME_BUDGET_BYTES} by ${overage} bytes — will be ` +
      `fragmented on LoRa (SPEC §6.2.3) and may exceed device retry timeouts.`,
  );
  return true;
}
