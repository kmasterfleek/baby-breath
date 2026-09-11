//! Display push: stream normalised CSI "display rows" to a node with a screen.
//!
//! For every CSI frame the server attributes to a node it keeps, per node and
//! per subcarrier bin, a running mean (EMA, ~10 s time constant) of the
//! amplitude — exactly the normalisation the browser page `cohort.html` uses
//! for its scrolling heatmaps. At most `--display-hz` times per second per
//! node it emits one tiny UDP packet holding the current row of 8-bit values
//! so the display firmware can draw the same heatmaps on its own screen.
//!
//! Wire format (little-endian, must match the firmware):
//!
//! | offset | size   | field                                              |
//! |--------|--------|----------------------------------------------------|
//! | 0      | 4      | magic `0xC511_0010` (u32 LE)                       |
//! | 4      | 1      | node id of the node this row describes             |
//! | 5      | 1      | n_bins: count of non-zero amplitude bins (≤ 64)    |
//! | 6      | 1      | flags: bit0 = node active (seen within 5 s)        |
//! | 7      | 1      | reserved (0)                                       |
//! | 8      | n_bins | u8 values, ascending bin index among non-zero bins |
//!
//! value = clamp(round(128 + (a / ema − 1) · 128 / 0.40), 0, 255), so ±40 %
//! around the running mean spans the full range. Until a node has fed the EMA
//! at least 20 samples every value is 128 (neutral).

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use tokio::net::UdpSocket;
use tokio::sync::mpsc;
use tracing::{info, warn};

use crate::{NodeState, SharedState};

/// Packet magic, written as u32 little-endian.
pub const DISPLAY_MAGIC: u32 = 0xC511_0010;
/// Packet header size in bytes.
pub const HEADER_LEN: usize = 8;
/// Maximum number of bins carried in one row.
pub const MAX_BINS: usize = 64;
/// Time constant of the per-bin running mean (mirrors `HEAT_MEAN_TAU_MS`).
pub const EMA_TAU: Duration = Duration::from_secs(10);
/// ±fraction of the running mean that spans the full 0..255 range.
pub const RANGE: f64 = 0.40;
/// Samples the EMA must have seen before rows carry real values.
pub const WARMUP_SAMPLES: u32 = 20;
/// A node is "active" when its previous frame arrived within this window.
pub const ACTIVE_WINDOW: Duration = Duration::from_secs(5);
/// Largest dt fed into the EMA update (mirrors the 2 s clamp in cohort.html).
const MAX_DT: Duration = Duration::from_secs(2);
/// Packets queued for the sender before new rows are dropped.
const QUEUE_LEN: usize = 64;
/// Minimum spacing between send-failure warnings.
const WARN_INTERVAL: Duration = Duration::from_secs(60);
/// How often the sender re-reads the display node's last known address.
const DEST_REFRESH: Duration = Duration::from_secs(1);

/// Map one amplitude against its running mean onto a u8 (128 = at the mean).
pub fn normalise(amp: f64, ema: f64) -> u8 {
    if !(ema > 0.0) || !amp.is_finite() {
        return 128;
    }
    let v = 128.0 + (amp / ema - 1.0) * (128.0 / RANGE);
    v.round().clamp(0.0, 255.0) as u8
}

/// Serialise one display row. `values` is truncated to [`MAX_BINS`].
pub fn build_packet(node_id: u8, active: bool, values: &[u8]) -> Vec<u8> {
    let n = values.len().min(MAX_BINS);
    let mut pkt = Vec::with_capacity(HEADER_LEN + n);
    pkt.extend_from_slice(&DISPLAY_MAGIC.to_le_bytes());
    pkt.push(node_id);
    pkt.push(n as u8);
    pkt.push(u8::from(active));
    pkt.push(0);
    pkt.extend_from_slice(&values[..n]);
    pkt
}

/// Pick at most [`MAX_BINS`] indices, evenly spaced, preserving ascending order.
fn select_bins(nonzero: &[usize]) -> Vec<usize> {
    if nonzero.len() <= MAX_BINS {
        return nonzero.to_vec();
    }
    (0..MAX_BINS)
        .map(|k| nonzero[k * nonzero.len() / MAX_BINS])
        .collect()
}

/// Per-node running-mean state.
#[derive(Debug, Default)]
struct NodeRows {
    /// EMA per bin index; 0.0 means "not yet initialised".
    ema: Vec<f64>,
    samples: u32,
    last_update: Option<Instant>,
    last_sent: Option<Instant>,
}

/// Pure row normaliser + rate limiter (no I/O). One instance serves all nodes.
#[derive(Debug)]
pub struct DisplayRows {
    min_interval: Duration,
    nodes: HashMap<u8, NodeRows>,
}

impl DisplayRows {
    /// `hz` = max rows per second per node (0 is treated as 1).
    pub fn new(hz: u32) -> Self {
        Self {
            min_interval: Duration::from_secs_f64(1.0 / f64::from(hz.max(1))),
            nodes: HashMap::new(),
        }
    }

    /// Feed one amplitude vector for `node_id`. Updates the running means and
    /// returns a packet when at least `1/hz` has elapsed since the last one.
    pub fn observe(&mut self, node_id: u8, amps: &[f64], now: Instant) -> Option<Vec<u8>> {
        let st = self.nodes.entry(node_id).or_default();
        if st.ema.len() < amps.len() {
            st.ema.resize(amps.len(), 0.0);
        }
        let dt = st
            .last_update
            .map(|t| now.saturating_duration_since(t).min(MAX_DT))
            .unwrap_or(Duration::ZERO);
        let alpha = 1.0 - (-dt.as_secs_f64() / EMA_TAU.as_secs_f64()).exp();
        let active = st
            .last_update
            .is_none_or(|t| now.saturating_duration_since(t) <= ACTIVE_WINDOW);
        let warm = st.samples >= WARMUP_SAMPLES;

        let nonzero: Vec<usize> = (0..amps.len()).filter(|&i| amps[i] != 0.0).collect();
        let bins = select_bins(&nonzero);
        let mut values = Vec::with_capacity(bins.len());
        for &i in &nonzero {
            let a = amps[i];
            let m = st.ema[i];
            if bins.binary_search(&i).is_ok() {
                values.push(if warm { normalise(a, m) } else { 128 });
            }
            st.ema[i] = if m > 0.0 { m + alpha * (a - m) } else { a };
        }
        st.samples = st.samples.saturating_add(1);
        st.last_update = Some(now);

        let due = st
            .last_sent
            .is_none_or(|t| now.saturating_duration_since(t) >= self.min_interval);
        if !due || values.is_empty() {
            return None;
        }
        st.last_sent = Some(now);
        Some(build_packet(node_id, active, &values))
    }
}

/// Non-blocking front half: normalises rows and queues packets for the sender.
pub struct DisplayPush {
    rows: DisplayRows,
    tx: mpsc::Sender<Vec<u8>>,
}

impl DisplayPush {
    /// Create the normaliser and the channel feeding [`sender_task`].
    pub fn new(hz: u32) -> (Self, mpsc::Receiver<Vec<u8>>) {
        let (tx, rx) = mpsc::channel(QUEUE_LEN);
        (Self { rows: DisplayRows::new(hz), tx }, rx)
    }

    /// Observe one frame; never blocks (rows are dropped if the queue is full).
    pub fn observe(&mut self, node_id: u8, amps: &[f64], now: Instant) {
        if let Some(pkt) = self.rows.observe(node_id, amps, now) {
            let _ = self.tx.try_send(pkt);
        }
    }
}

/// Where rows go: the display node's last known source IP on `port`.
pub fn destination(nodes: &HashMap<u8, NodeState>, display_node: u8, port: u16) -> Option<SocketAddr> {
    nodes
        .get(&display_node)
        .and_then(|ns| ns.last_addr)
        .map(|a| SocketAddr::new(a.ip(), port))
}

/// Background task: drain queued rows and send them to the display node.
///
/// Rows are dropped silently until the display node has been heard from.
pub async fn sender_task(
    state: SharedState,
    mut rx: mpsc::Receiver<Vec<u8>>,
    display_node: u8,
    port: u16,
    hz: u32,
) {
    let socket = match UdpSocket::bind("0.0.0.0:0").await {
        Ok(s) => s,
        Err(e) => {
            warn!("Display push disabled: failed to bind UDP socket: {e}");
            return;
        }
    };
    info!("Display push → node {display_node} at {hz} Hz (UDP port {port})");

    let mut dest: Option<SocketAddr> = None;
    let mut last_lookup: Option<Instant> = None;
    let mut send_errors: u64 = 0;
    let mut last_warn: Option<Instant> = None;

    while let Some(pkt) = rx.recv().await {
        let now = Instant::now();
        let stale = last_lookup.is_none_or(|t| now.duration_since(t) >= DEST_REFRESH);
        if dest.is_none() || stale {
            let found = destination(&state.read().await.node_states, display_node, port);
            last_lookup = Some(now);
            if let Some(a) = found {
                if dest.is_none() {
                    info!("Display push: node {display_node} found at {} — sending rows to {a}", a.ip());
                }
                dest = Some(a);
            }
        }
        let Some(addr) = dest else { continue };
        if let Err(e) = socket.send_to(&pkt, addr).await {
            send_errors += 1;
            if last_warn.is_none_or(|t| now.duration_since(t) >= WARN_INTERVAL) {
                warn!("Display push send to {addr} failed ({send_errors} errors so far): {e}");
                last_warn = Some(now);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ms(n: u64) -> Duration {
        Duration::from_millis(n)
    }

    /// Warm a node up with `n` identical frames spaced `gap` apart; returns the next Instant.
    fn warm(rows: &mut DisplayRows, node: u8, amps: &[f64], n: u32, gap: Duration) -> Instant {
        let mut t = Instant::now();
        for _ in 0..n {
            rows.observe(node, amps, t);
            t += gap;
        }
        t
    }

    #[test]
    fn packet_layout_matches_wire_format() {
        let pkt = build_packet(7, true, &[0, 128, 255]);
        assert_eq!(&pkt[0..4], &[0x10, 0x00, 0x11, 0xC5]);
        assert_eq!(pkt[4], 7);
        assert_eq!(pkt[5], 3);
        assert_eq!(pkt[6], 1);
        assert_eq!(pkt[7], 0);
        assert_eq!(&pkt[8..], &[0, 128, 255]);
        assert_eq!(pkt.len(), HEADER_LEN + 3);

        let inactive = build_packet(2, false, &[1; 100]);
        assert_eq!(inactive[5], 64, "n_bins is capped at 64");
        assert_eq!(inactive[6], 0);
        assert_eq!(inactive.len(), HEADER_LEN + 64);
    }

    #[test]
    fn zero_bins_are_excluded_and_order_is_ascending() {
        let mut rows = DisplayRows::new(15);
        let amps = [0.0, 10.0, 0.0, 20.0, 30.0, 0.0];
        let pkt = rows.observe(3, &amps, Instant::now()).expect("first frame emits a row");
        assert_eq!(pkt[4], 3);
        assert_eq!(pkt[5], 3, "three non-zero bins");
        assert_eq!(pkt[6] & 1, 1, "node just seen is active");
        assert_eq!(&pkt[8..], &[128, 128, 128], "warm-up rows are neutral");
    }

    #[test]
    fn normalisation_maps_plus_minus_forty_percent_to_full_range() {
        assert_eq!(normalise(14.0, 10.0), 255);
        assert_eq!(normalise(6.0, 10.0), 0);
        assert_eq!(normalise(10.0, 10.0), 128);
        assert_eq!(normalise(12.0, 10.0), 192);
        assert_eq!(normalise(100.0, 10.0), 255, "clamped high");
        assert_eq!(normalise(0.5, 10.0), 0, "clamped low");
        assert_eq!(normalise(5.0, 0.0), 128, "no mean yet → neutral");
    }

    #[test]
    fn first_nineteen_samples_emit_neutral_then_real_values() {
        let mut rows = DisplayRows::new(1000);
        let base = [10.0, 0.0, 10.0];
        let mut t = Instant::now();
        for k in 0..WARMUP_SAMPLES {
            let pkt = rows.observe(1, &base, t).expect("row every frame at 1000 Hz");
            assert_eq!(&pkt[8..], &[128, 128], "sample {k} is still warm-up");
            t += ms(50);
        }
        // EMA has 20 identical samples → exactly 10.0. Same amplitude → 128.
        let pkt = rows.observe(1, &base, t).unwrap();
        assert_eq!(&pkt[8..], &[128, 128]);
        t += ms(50);
        // +40 % on bin 0 → 255, −40 % on bin 2 → 0 (normalised against pre-update EMA).
        let pkt = rows.observe(1, &[14.0, 0.0, 6.0], t).unwrap();
        assert_eq!(&pkt[8..], &[255, 0]);
    }

    #[test]
    fn ema_tracks_the_running_mean_with_ten_second_tau() {
        let mut rows = DisplayRows::new(1000);
        let t = warm(&mut rows, 1, &[10.0], WARMUP_SAMPLES, ms(50));
        // `warm` returns 50 ms past the last frame; step exactly 1 s from it.
        // alpha = 1 − e^(−1/10) ≈ 0.0952 → ema ≈ 10.95 after one 20.0 sample.
        rows.observe(1, &[20.0], t - ms(50) + Duration::from_secs(1));
        let ema = rows.nodes[&1].ema[0];
        assert!((ema - 10.9516).abs() < 1e-3, "ema was {ema}");
    }

    #[test]
    fn rate_limit_emits_one_packet_for_two_frames_ten_ms_apart() {
        let mut rows = DisplayRows::new(15);
        let t0 = Instant::now();
        assert!(rows.observe(1, &[1.0, 2.0], t0).is_some());
        assert!(rows.observe(1, &[1.0, 2.0], t0 + ms(10)).is_none());
        assert!(rows.observe(1, &[1.0, 2.0], t0 + ms(67)).is_some(), "due after 1/15 s");
        // Rate limiting is per node: another node is not throttled by node 1.
        assert!(rows.observe(2, &[1.0], t0 + ms(70)).is_some());
    }

    #[test]
    fn node_is_flagged_inactive_after_a_long_gap() {
        let mut rows = DisplayRows::new(15);
        let t0 = Instant::now();
        rows.observe(1, &[1.0], t0);
        let pkt = rows.observe(1, &[1.0], t0 + Duration::from_secs(6)).unwrap();
        assert_eq!(pkt[6] & 1, 0);
        let pkt = rows.observe(1, &[1.0], t0 + Duration::from_secs(7)).unwrap();
        assert_eq!(pkt[6] & 1, 1);
    }

    #[test]
    fn more_than_64_bins_are_decimated_in_order() {
        let mut rows = DisplayRows::new(15);
        let amps: Vec<f64> = (0..128).map(|i| 1.0 + i as f64).collect();
        let pkt = rows.observe(1, &amps, Instant::now()).unwrap();
        assert_eq!(pkt[5], 64);
        assert_eq!(pkt.len(), HEADER_LEN + 64);
        let picked = select_bins(&(0..128).collect::<Vec<_>>());
        assert!(picked.windows(2).all(|w| w[0] < w[1]));
        assert_eq!(picked[0], 0);
        assert_eq!(picked[63], 126);
    }

    #[test]
    fn destination_uses_display_node_ip_with_display_port() {
        let mut nodes: HashMap<u8, NodeState> = HashMap::new();
        let mut ns = NodeState::new();
        ns.last_addr = Some("192.168.1.42:5005".parse().unwrap());
        nodes.insert(2, ns);
        nodes.insert(1, NodeState::new());

        assert_eq!(destination(&nodes, 2, 5006), Some("192.168.1.42:5006".parse().unwrap()));
        assert_eq!(destination(&nodes, 1, 5006), None, "known node, address not yet learned");
        assert_eq!(destination(&nodes, 9, 5006), None, "unknown node");
    }
}
