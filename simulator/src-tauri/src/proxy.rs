//! SIM-006 — Proxy mode (simulated LoRa).
//!
//! Proxy mode sits **on top** of direct mode's HTTP transport. The real
//! HTTP fetch still talks to the configured base URL (typically a local
//! `pagerctl dev` server); proxy mode just overlays LoRa-shaped behavior
//! — artificial latency, per-fragment loss with single retry, and
//! fragmentation accounting — before the rendered Frame is handed back
//! to the frontend. The transport surface in `direct.rs` is unchanged;
//! the controller here is consulted from `lib.rs::direct_fetch` after
//! the HTTP response lands.
//!
//! Why a controller in front of the HTTP client rather than a different
//! `reqwest::Client`: the developer's app server is still ordinary HTTP
//! whether you're "going over LoRa" or not in the simulator — what we're
//! simulating is the delivery characteristics, not the bytes on the
//! wire. Pushing this logic up to the SIM crate also keeps the proxy
//! easy to inspect from the SIM-005 network panel (a single HTTP
//! exchange per device fetch, plus a `ProxyOutcome` annotation).
//!
//! Numbers driving this module come from `SPEC.md`:
//! - §14 — "Encoded Frame size (LoRa-targeted) < 200 B | > 250 B (must
//!   fragment)" → MTU defaults to 250 B.
//! - §6.2.3 — "Frames larger than the LoRa MTU are split into numbered
//!   fragments … Reassembled by recipient. Lost fragments retried
//!   individually." → both directions fragment; each lost fragment
//!   pays one extra latency unit for a single retry.
//! - SIM-006 acceptance — "Latency slider 0-30 s; loss rate 0-50%;
//!   reproduces fragmentation behavior."
//!
//! Determinism: the RNG is seedable so tests can reproduce a loss
//! pattern exactly. In production we seed from the wall clock at
//! controller construction; tests reach in via `from_seed`.

use std::sync::Mutex;

use serde::{Deserialize, Serialize};

/// LoRa MTU in bytes. `SPEC.md` §14 calls out 250 B as the boundary
/// past which frames must fragment. Kept tunable because firmware may
/// settle on a slightly different number after measurement; tests
/// override it to keep their fragment math obvious.
pub const DEFAULT_MTU_BYTES: usize = 250;

/// Slider ceiling for the latency control (`SPEC.md`-driven: SIM-006
/// acceptance pins this at 30 s). Inputs above this saturate so a
/// fat-fingered slider can't park the simulator in an unusable state.
pub const MAX_LATENCY_MS: u32 = 30_000;

/// Slider ceiling for the loss control (SIM-006 acceptance: 50 %).
pub const MAX_LOSS_RATE: f32 = 0.5;

/// Live proxy parameters. Mirrors what the slider UI controls; the
/// frontend reads this back to keep its inputs in sync after any
/// clamp the backend applied.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct ProxyConfig {
    /// Master toggle. When `false`, the controller is a no-op and
    /// `direct_fetch` behaves exactly as before. Persisting the toggle
    /// separately from the sliders means the engineer can park a known
    /// latency/loss profile and flip it on/off without losing values.
    pub enabled: bool,
    /// Baseline round-trip latency in milliseconds. Interpreted as the
    /// total for a "single fragment each way" exchange (req frag +
    /// resp frag); larger payloads scale linearly with fragment count,
    /// and each retry adds one fragment worth. Saturated to
    /// `MAX_LATENCY_MS`.
    pub latency_ms: u32,
    /// Per-fragment drop probability, 0.0..=`MAX_LOSS_RATE`.
    pub loss_rate: f32,
    /// LoRa MTU in bytes. Defaults to `DEFAULT_MTU_BYTES`; clamped to
    /// `>= 1` so the fragment math never divides by zero.
    pub mtu_bytes: u32,
}

impl Default for ProxyConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            latency_ms: 0,
            loss_rate: 0.0,
            mtu_bytes: DEFAULT_MTU_BYTES as u32,
        }
    }
}

impl ProxyConfig {
    /// Clamp sliders to their advertised ranges. The frontend already
    /// clamps via `min`/`max` attributes; we re-clamp here so a buggy
    /// caller (or a hand-rolled command-line script) can't push the
    /// simulator outside the modeled envelope.
    pub fn normalized(mut self) -> Self {
        if self.latency_ms > MAX_LATENCY_MS {
            self.latency_ms = MAX_LATENCY_MS;
        }
        if !self.loss_rate.is_finite() || self.loss_rate < 0.0 {
            self.loss_rate = 0.0;
        }
        if self.loss_rate > MAX_LOSS_RATE {
            self.loss_rate = MAX_LOSS_RATE;
        }
        if self.mtu_bytes < 1 {
            self.mtu_bytes = 1;
        }
        self
    }

    /// Total fragment count for one round trip. Both directions are
    /// fragmented separately (`SPEC.md` §6.2.3); each direction has at
    /// least one fragment so a zero-byte body still costs one airtime
    /// unit (the empty request still has to traverse the mesh).
    pub fn fragment_count(&self, request_bytes: usize, response_bytes: usize) -> u32 {
        let mtu = self.mtu_bytes.max(1) as usize;
        let req = request_bytes.max(1).div_ceil(mtu);
        let resp = response_bytes.max(1).div_ceil(mtu);
        (req + resp) as u32
    }
}

/// Result of one simulated exchange. Returned alongside the rendered
/// frame so the frontend can show the engineer what the slider profile
/// "cost" this request.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct ProxyOutcome {
    /// Total number of fragments that the request + response decomposed
    /// into before retries.
    pub total_fragments: u32,
    /// How many of those fragments were dropped by the loss simulation.
    /// Each adds one retry (single-shot, matching SPEC §6.2.3's "retried
    /// individually" wording).
    pub fragments_lost: u32,
    /// Total fragments actually transmitted = `total_fragments +
    /// fragments_lost` (each dropped fragment retried exactly once).
    pub fragments_sent: u32,
    /// Total simulated wall-clock cost in milliseconds. The simulator
    /// sleeps for exactly this long after the underlying HTTP fetch
    /// returns; status bar reports it back to the engineer.
    pub simulated_latency_ms: u64,
}

impl ProxyConfig {
    /// Sample one round of fragmentation + loss. Pure function over
    /// `rng` so tests reproduce a loss pattern by reusing a seeded RNG.
    ///
    /// Latency model: `latency_ms` is the baseline for the smallest
    /// possible exchange (1 req frag + 1 resp frag = 2 fragments).
    /// Larger payloads scale the airtime by fragment count; each lost
    /// fragment adds one retry's worth.
    pub fn simulate(
        &self,
        request_bytes: usize,
        response_bytes: usize,
        rng: &mut SplitMix64,
    ) -> ProxyOutcome {
        let total = self.fragment_count(request_bytes, response_bytes);
        let mut lost = 0u32;
        if self.loss_rate > 0.0 {
            for _ in 0..total {
                if rng.next_unit_f32() < self.loss_rate {
                    lost += 1;
                }
            }
        }
        let sent = total + lost;
        // 2 fragments = baseline. We measure airtime in "fragment units"
        // and divide the baseline latency by 2 so the slider semantics
        // match the documented "round trip" framing.
        let per_fragment_ms = u64::from(self.latency_ms) / 2;
        let simulated_latency_ms = u64::from(sent) * per_fragment_ms;
        ProxyOutcome {
            total_fragments: total,
            fragments_lost: lost,
            fragments_sent: sent,
            simulated_latency_ms,
        }
    }
}

/// Tauri-managed live state for proxy mode. Holds the config + a
/// process-lifetime RNG so per-fetch sampling is consistent across
/// commands.
pub struct ProxyController {
    inner: Mutex<Inner>,
}

struct Inner {
    config: ProxyConfig,
    rng: SplitMix64,
}

impl ProxyController {
    /// Production constructor — seeds the RNG from the wall clock so
    /// successive runs see different loss patterns.
    pub fn new() -> Self {
        Self::from_seed(seed_from_clock())
    }

    /// Test / deterministic constructor. The seed fully determines the
    /// sequence of fragment-loss decisions across calls.
    pub fn from_seed(seed: u64) -> Self {
        Self {
            inner: Mutex::new(Inner {
                config: ProxyConfig::default(),
                rng: SplitMix64::from_seed(seed),
            }),
        }
    }

    /// Replace the live config. The submitted config is clamped to its
    /// advertised range; the clamped value is returned so the caller
    /// can reconcile its slider state without a follow-up read.
    pub fn set_config(&self, config: ProxyConfig) -> ProxyConfig {
        let normalized = config.normalized();
        let mut inner = self.lock();
        inner.config = normalized;
        normalized
    }

    pub fn config(&self) -> ProxyConfig {
        self.lock().config
    }

    /// If proxy mode is enabled, sample one round of fragment / loss
    /// behavior and return the resulting outcome. Returns `None` when
    /// proxy mode is off so the call site can skip the sleep entirely.
    pub fn sample(&self, request_bytes: usize, response_bytes: usize) -> Option<ProxyOutcome> {
        let mut inner = self.lock();
        if !inner.config.enabled {
            return None;
        }
        let config = inner.config;
        Some(config.simulate(request_bytes, response_bytes, &mut inner.rng))
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().expect("proxy controller poisoned")
    }
}

impl Default for ProxyController {
    fn default() -> Self {
        Self::new()
    }
}

/// Tiny SplitMix64 PRNG. Self-contained so the simulator doesn't pull
/// in `rand` just to flip coins for fragment loss. Good enough for
/// uniform sampling at this scale; the algorithm is well-known and
/// deterministic when seeded.
pub struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    pub fn from_seed(seed: u64) -> Self {
        Self { state: seed }
    }

    pub fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform sample in `[0.0, 1.0)`. Uses the top 24 bits of one
    /// `u64` draw to land in `f32`'s mantissa exactly — no rounding
    /// surprises near the boundaries.
    pub fn next_unit_f32(&mut self) -> f32 {
        const MANTISSA_BITS: u32 = 24;
        const DENOM: f32 = (1u32 << MANTISSA_BITS) as f32;
        ((self.next_u64() >> (64 - MANTISSA_BITS)) as f32) / DENOM
    }
}

fn seed_from_clock() -> u64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        // System-time unavailable is vanishingly unlikely on a desktop
        // host; fall back to a fixed value so the controller still
        // works rather than blowing up at startup.
        .unwrap_or(0xDEAD_BEEF_CAFE_F00D)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_config_is_disabled_no_op() {
        let cfg = ProxyConfig::default();
        assert!(!cfg.enabled);
        assert_eq!(cfg.latency_ms, 0);
        assert_eq!(cfg.loss_rate, 0.0);
        assert_eq!(cfg.mtu_bytes, DEFAULT_MTU_BYTES as u32);
    }

    #[test]
    fn fragment_count_smallest_exchange_is_two() {
        // Empty request, empty response — still costs two fragments
        // (one each way), per the model.
        let cfg = ProxyConfig::default();
        assert_eq!(cfg.fragment_count(0, 0), 2);
        assert_eq!(cfg.fragment_count(1, 1), 2);
    }

    #[test]
    fn fragment_count_scales_with_payload_size() {
        let cfg = ProxyConfig {
            mtu_bytes: 100,
            ..ProxyConfig::default()
        };
        // 250 / 100 = ceil(2.5) = 3 resp frags; 50 / 100 = ceil(0.5) = 1 req frag.
        assert_eq!(cfg.fragment_count(50, 250), 4);
        // 100 / 100 = 1 frag each side.
        assert_eq!(cfg.fragment_count(100, 100), 2);
        // 101 / 100 spills into a second fragment.
        assert_eq!(cfg.fragment_count(101, 101), 4);
    }

    #[test]
    fn fragment_count_treats_zero_mtu_safely() {
        // Pathological input — `normalized` would clamp to 1 first, but
        // `fragment_count` itself must not panic if called raw.
        let cfg = ProxyConfig {
            mtu_bytes: 0,
            ..ProxyConfig::default()
        };
        assert!(cfg.fragment_count(10, 10) >= 2);
    }

    #[test]
    fn normalize_clamps_out_of_range_inputs() {
        let cfg = ProxyConfig {
            enabled: true,
            latency_ms: 99_999,
            loss_rate: 1.5,
            mtu_bytes: 0,
        }
        .normalized();
        assert_eq!(cfg.latency_ms, MAX_LATENCY_MS);
        assert_eq!(cfg.loss_rate, MAX_LOSS_RATE);
        assert_eq!(cfg.mtu_bytes, 1);
    }

    #[test]
    fn normalize_replaces_non_finite_loss_rate() {
        let cfg = ProxyConfig {
            loss_rate: f32::NAN,
            ..ProxyConfig::default()
        }
        .normalized();
        assert_eq!(cfg.loss_rate, 0.0);
    }

    #[test]
    fn zero_loss_returns_no_drops_regardless_of_rng() {
        let cfg = ProxyConfig {
            enabled: true,
            latency_ms: 1000,
            loss_rate: 0.0,
            mtu_bytes: 250,
        };
        let mut rng = SplitMix64::from_seed(0xABCD);
        let out = cfg.simulate(100, 100, &mut rng);
        assert_eq!(out.total_fragments, 2);
        assert_eq!(out.fragments_lost, 0);
        assert_eq!(out.fragments_sent, 2);
        // 2 fragments × (1000 / 2) ms each = 1000 ms total.
        assert_eq!(out.simulated_latency_ms, 1000);
    }

    #[test]
    fn high_loss_eventually_drops_a_fragment() {
        // With 50% loss and 100 fragments, the chance of zero drops is
        // 0.5^100 — astronomically small. Any working RNG hits.
        let cfg = ProxyConfig {
            enabled: true,
            latency_ms: 0,
            loss_rate: 0.5,
            mtu_bytes: 10,
        };
        let mut rng = SplitMix64::from_seed(0xCAFE);
        let out = cfg.simulate(1000, 0, &mut rng);
        // 1000B / 10B = 100 req frags + 1 resp frag = 101 frags.
        assert_eq!(out.total_fragments, 101);
        assert!(out.fragments_lost > 0, "expected at least one drop, got {out:?}");
        assert!(out.fragments_lost <= out.total_fragments);
        assert_eq!(out.fragments_sent, out.total_fragments + out.fragments_lost);
    }

    #[test]
    fn latency_scales_with_fragment_count() {
        let cfg = ProxyConfig {
            enabled: true,
            latency_ms: 2000,
            loss_rate: 0.0,
            mtu_bytes: 100,
        };
        let mut rng = SplitMix64::from_seed(1);
        // Small exchange: 2 fragments total → full baseline.
        let small = cfg.simulate(10, 10, &mut rng);
        assert_eq!(small.simulated_latency_ms, 2000);
        // Big exchange: 1KB req (10 frags) + 1KB resp (10 frags) = 20 frags total
        //              → 10× baseline.
        let big = cfg.simulate(1000, 1000, &mut rng);
        assert_eq!(big.simulated_latency_ms, 20_000);
    }

    #[test]
    fn deterministic_replay_with_same_seed() {
        // Same seed + same call sequence + same config → same outcome.
        // Guards against accidentally introducing nondeterminism (e.g.
        // hashing into the RNG path) in a future refactor.
        let cfg = ProxyConfig {
            enabled: true,
            latency_ms: 1000,
            loss_rate: 0.3,
            mtu_bytes: 50,
        };
        let mut rng_a = SplitMix64::from_seed(42);
        let mut rng_b = SplitMix64::from_seed(42);
        let a = cfg.simulate(200, 500, &mut rng_a);
        let b = cfg.simulate(200, 500, &mut rng_b);
        assert_eq!(a, b);
    }

    #[test]
    fn controller_skips_sampling_when_disabled() {
        let ctrl = ProxyController::from_seed(0);
        // Default config has enabled=false.
        assert!(ctrl.sample(100, 100).is_none());
        ctrl.set_config(ProxyConfig {
            enabled: true,
            latency_ms: 500,
            loss_rate: 0.0,
            mtu_bytes: 250,
        });
        assert!(ctrl.sample(100, 100).is_some());
    }

    #[test]
    fn controller_clamps_and_returns_normalized() {
        let ctrl = ProxyController::from_seed(0);
        let returned = ctrl.set_config(ProxyConfig {
            enabled: true,
            latency_ms: 60_000,
            loss_rate: 0.9,
            mtu_bytes: 0,
        });
        assert_eq!(returned.latency_ms, MAX_LATENCY_MS);
        assert_eq!(returned.loss_rate, MAX_LOSS_RATE);
        assert_eq!(returned.mtu_bytes, 1);
        // Subsequent reads see the clamped value.
        assert_eq!(ctrl.config(), returned);
    }

    #[test]
    fn split_mix64_uniform_in_unit_interval() {
        let mut rng = SplitMix64::from_seed(0);
        for _ in 0..1000 {
            let v = rng.next_unit_f32();
            assert!((0.0..1.0).contains(&v), "out of range: {v}");
        }
    }
}
