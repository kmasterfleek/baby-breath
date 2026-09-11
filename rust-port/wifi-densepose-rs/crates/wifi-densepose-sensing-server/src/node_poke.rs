//! Node poke: keep ESP32 CSI flowing by generating downlink traffic.
//!
//! ESP32 nodes only produce CSI when the access point sends them downlink
//! frames. With nothing else on the network talking to a node, CSI decays to
//! roughly 0.1 Hz. Sending a 1-byte UDP datagram to each active node at
//! ~20 Hz makes the access point emit a frame per poke, which restores
//! 13-70 Hz CSI in measurements.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use tokio::net::UdpSocket;
use tracing::{info, warn};

use crate::{NodeState, SharedState};

/// Nodes not heard from within this window are no longer poked.
pub const POKE_MAX_AGE: Duration = Duration::from_secs(60);

/// Minimum spacing between send-failure warnings.
const WARN_INTERVAL: Duration = Duration::from_secs(60);

/// Pure selection of poke targets from `(last_addr, last_frame_time)` pairs.
///
/// A node is a target when it has a known source address and was last heard
/// from no more than `max_age` before `now`. Duplicate addresses (two node ids
/// behind one socket) are collapsed so each address is poked once per tick.
pub fn poke_targets<I>(nodes: I, now: Instant, max_age: Duration) -> Vec<SocketAddr>
where
    I: IntoIterator<Item = (Option<SocketAddr>, Option<Instant>)>,
{
    let mut out: Vec<SocketAddr> = Vec::new();
    for (addr, seen) in nodes {
        let (Some(addr), Some(seen)) = (addr, seen) else {
            continue;
        };
        if now.saturating_duration_since(seen) > max_age {
            continue;
        }
        if !out.contains(&addr) {
            out.push(addr);
        }
    }
    out
}

/// Poke targets drawn from the server's per-node state map.
pub fn node_poke_targets(
    nodes: &HashMap<u8, NodeState>,
    now: Instant,
    max_age: Duration,
) -> Vec<SocketAddr> {
    poke_targets(
        nodes.values().map(|ns| (ns.last_addr, ns.last_frame_time)),
        now,
        max_age,
    )
}

/// Background task: send a 1-byte UDP datagram to every active node at `hz`.
///
/// Returns immediately when `hz == 0` or the socket cannot be bound. Send
/// errors are counted and reported at most once per [`WARN_INTERVAL`].
pub async fn node_poke_task(state: SharedState, hz: u32) {
    if hz == 0 {
        return;
    }
    let socket = match UdpSocket::bind("0.0.0.0:0").await {
        Ok(s) => s,
        Err(e) => {
            warn!("Node poke disabled: failed to bind UDP socket: {e}");
            return;
        }
    };
    info!("Node poke active at {hz} Hz");

    let mut interval = tokio::time::interval(Duration::from_secs_f64(1.0 / f64::from(hz)));
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

    let mut send_errors: u64 = 0;
    let mut last_warn: Option<Instant> = None;

    loop {
        interval.tick().await;

        let targets = {
            let s = state.read().await;
            node_poke_targets(&s.node_states, Instant::now(), POKE_MAX_AGE)
        };

        for addr in targets {
            if let Err(e) = socket.send_to(&[0u8], addr).await {
                send_errors += 1;
                let now = Instant::now();
                let due = last_warn.is_none_or(|t| now.duration_since(t) >= WARN_INTERVAL);
                if due {
                    warn!("Node poke send to {addr} failed ({send_errors} errors so far): {e}");
                    last_warn = Some(now);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn addr(port: u16) -> SocketAddr {
        format!("192.168.1.{}:{port}", port % 250 + 1).parse().unwrap()
    }

    #[test]
    fn selects_only_recent_nodes_with_addresses() {
        let now = Instant::now();
        let fresh = now - Duration::from_secs(5);
        let stale = now - Duration::from_secs(120);
        let nodes = vec![
            (Some(addr(5005)), Some(fresh)),  // recent: poke
            (Some(addr(5006)), Some(stale)),  // too old: skip
            (None, Some(fresh)),              // no address: skip
            (Some(addr(5007)), None),         // never seen: skip
        ];

        let targets = poke_targets(nodes, now, POKE_MAX_AGE);

        assert_eq!(targets, vec![addr(5005)]);
    }

    #[test]
    fn boundary_age_is_inclusive_and_duplicates_collapse() {
        let now = Instant::now();
        let max_age = Duration::from_secs(60);
        let at_limit = now - max_age;
        let nodes = vec![
            (Some(addr(5005)), Some(at_limit)),
            (Some(addr(5005)), Some(now)),
            (Some(addr(5005)), Some(now - max_age - Duration::from_millis(1))),
        ];

        let targets = poke_targets(nodes, now, max_age);

        assert_eq!(targets, vec![addr(5005)]);
    }

    #[test]
    fn future_timestamps_do_not_panic() {
        let now = Instant::now();
        let nodes = vec![(Some(addr(5005)), Some(now + Duration::from_secs(10)))];
        assert_eq!(poke_targets(nodes, now, POKE_MAX_AGE), vec![addr(5005)]);
    }

    #[test]
    fn node_state_map_is_read_correctly() {
        let now = Instant::now();
        let mut nodes: HashMap<u8, NodeState> = HashMap::new();

        let mut active = NodeState::new();
        active.last_addr = Some(addr(5005));
        active.last_frame_time = Some(now);
        nodes.insert(1, active);

        let mut silent = NodeState::new();
        silent.last_addr = Some(addr(5006));
        silent.last_frame_time = Some(now - Duration::from_secs(600));
        nodes.insert(2, silent);

        nodes.insert(3, NodeState::new());

        let targets = node_poke_targets(&nodes, now, POKE_MAX_AGE);

        assert_eq!(targets, vec![addr(5005)]);
    }
}
