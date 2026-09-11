//! Bridge between sensing-server frame data and signal crate FieldModel
//! for eigenvalue-based person counting.
//!
//! The FieldModel decomposes CSI observations into environmental drift and
//! body perturbation via SVD eigenmodes. When calibrated, perturbation energy
//! provides a physics-grounded occupancy estimate that supplements the
//! score-based heuristic in `score_to_person_count`.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use wifi_densepose_signal::ruvsense::field_model::{
    CalibrationStatus, FieldModel, FieldModelConfig, FieldModelError,
};

use super::score_to_person_count;

/// Number of recent frames to feed into perturbation extraction.
const OCCUPANCY_WINDOW: usize = 50;

/// Perturbation energy threshold for detecting a second person.
const ENERGY_THRESH_2: f64 = 12.0;
/// Perturbation energy threshold for detecting a third person.
const ENERGY_THRESH_3: f64 = 25.0;

/// Create a FieldModelConfig for single-link mode (one ESP32 node = one link).
/// This avoids the DimensionMismatch error when feeding single-frame observations.
pub fn single_link_config() -> FieldModelConfig {
    FieldModelConfig {
        n_links: 1,
        ..FieldModelConfig::default()
    }
}

/// Estimate occupancy using the FieldModel when calibrated, falling back
/// to the score-based heuristic otherwise.
///
/// Prefers `estimate_occupancy()` (eigenvalue-based) when the model is
/// calibrated and enough frames are available. Falls back to perturbation
/// energy thresholds, then to the score heuristic.
pub fn occupancy_or_fallback(
    field: &FieldModel,
    frame_history: &VecDeque<Vec<f64>>,
    smoothed_score: f64,
    prev_count: usize,
) -> usize {
    match field.status() {
        CalibrationStatus::Fresh | CalibrationStatus::Stale => {
            let frames: Vec<Vec<f64>> = frame_history
                .iter()
                .rev()
                .take(OCCUPANCY_WINDOW)
                .cloned()
                .collect();

            if frames.is_empty() {
                return score_to_person_count(smoothed_score, prev_count);
            }

            // Try eigenvalue-based occupancy first (best accuracy).
            match field.estimate_occupancy(&frames) {
                Ok(count) => return count,
                Err(_) => {} // fall through to perturbation energy
            }

            // Fallback: perturbation energy thresholds.
            // FieldModel expects [n_links][n_subcarriers] — we use n_links=1.
            let observation = vec![frames[0].clone()];
            match field.extract_perturbation(&observation) {
                Ok(perturbation) => {
                    if perturbation.total_energy > ENERGY_THRESH_3 {
                        3
                    } else if perturbation.total_energy > ENERGY_THRESH_2 {
                        2
                    } else if perturbation.total_energy > 1.0 {
                        1
                    } else {
                        0
                    }
                }
                Err(_) => score_to_person_count(smoothed_score, prev_count),
            }
        }
        _ => score_to_person_count(smoothed_score, prev_count),
    }
}

/// Set once the first calibration frame has been resampled, so the warning
/// is logged a single time rather than on every frame.
static RESAMPLE_WARNED: AtomicBool = AtomicBool::new(false);

/// Feed the latest frame to the FieldModel during calibration collection.
///
/// Acts while the model is `Uncalibrated` (fresh model, nothing fed yet) or
/// `Collecting`. A fresh `FieldModel` starts `Uncalibrated` and only becomes
/// `Collecting` inside `feed_calibration`, so gating on `Collecting` alone
/// would never collect anything.
///
/// The frame is wrapped as a single-link observation (n_links=1). If its
/// length differs from the model's subcarrier count (reported back via
/// `DimensionMismatch`), it is linearly resampled to that length and fed
/// again; the first resample is logged at warn level.
pub fn maybe_feed_calibration(field: &mut FieldModel, frame_history: &VecDeque<Vec<f64>>) {
    if !matches!(
        field.status(),
        CalibrationStatus::Uncalibrated | CalibrationStatus::Collecting
    ) {
        return;
    }
    let Some(latest) = frame_history.back() else {
        return;
    };
    // Single-link observation: [1][n_subcarriers]
    match field.feed_calibration(std::slice::from_ref(latest)) {
        Ok(()) => {}
        Err(FieldModelError::DimensionMismatch { expected, got }) if expected > 0 && got != expected => {
            if !RESAMPLE_WARNED.swap(true, Ordering::Relaxed) {
                tracing::warn!(
                    "FieldModel calibration: frame has {got} subcarriers, model expects {expected}; \
                     resampling (logged once)"
                );
            }
            let resampled = resample_linear(latest, expected);
            if let Err(e) = field.feed_calibration(&[resampled]) {
                tracing::debug!("FieldModel calibration feed after resample: {e}");
            }
        }
        Err(e) => tracing::debug!("FieldModel calibration feed: {e}"),
    }
}

/// Linearly resample `src` to `n` points, preserving both endpoints.
///
/// An empty `src` yields zeros; `n == 0` yields an empty vector.
pub fn resample_linear(src: &[f64], n: usize) -> Vec<f64> {
    if n == 0 {
        return Vec::new();
    }
    if src.is_empty() {
        return vec![0.0; n];
    }
    if src.len() == n {
        return src.to_vec();
    }
    if src.len() == 1 || n == 1 {
        return vec![src[0]; n];
    }
    let scale = (src.len() - 1) as f64 / (n - 1) as f64;
    (0..n)
        .map(|i| {
            let pos = i as f64 * scale;
            let lo = pos.floor() as usize;
            let hi = (lo + 1).min(src.len() - 1);
            let t = pos - lo as f64;
            src[lo] * (1.0 - t) + src[hi] * t
        })
        .collect()
}

/// Parse node positions from a semicolon-delimited string.
///
/// Format: `"x,y,z;x,y,z;..."` where each coordinate is an `f32`.
/// Malformed entries are skipped with a warning log.
pub fn parse_node_positions(input: &str) -> Vec<[f32; 3]> {
    if input.is_empty() {
        return Vec::new();
    }
    input
        .split(';')
        .enumerate()
        .filter_map(|(idx, triplet)| {
            let parts: Vec<&str> = triplet.split(',').collect();
            if parts.len() != 3 {
                tracing::warn!("Skipping malformed node position entry {idx}: '{triplet}' (expected x,y,z)");
                return None;
            }
            match (parts[0].parse::<f32>(), parts[1].parse::<f32>(), parts[2].parse::<f32>()) {
                (Ok(x), Ok(y), Ok(z)) => Some([x, y, z]),
                _ => {
                    tracing::warn!("Skipping unparseable node position entry {idx}: '{triplet}'");
                    None
                }
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_node_positions() {
        let positions = parse_node_positions("0,0,1.5;3,0,1.5;1.5,3,1.5");
        assert_eq!(positions.len(), 3);
        assert_eq!(positions[0], [0.0, 0.0, 1.5]);
        assert_eq!(positions[1], [3.0, 0.0, 1.5]);
        assert_eq!(positions[2], [1.5, 3.0, 1.5]);
    }

    #[test]
    fn test_parse_node_positions_empty() {
        let positions = parse_node_positions("");
        assert!(positions.is_empty());
    }

    #[test]
    fn test_parse_node_positions_invalid() {
        let positions = parse_node_positions("abc;1,2,3");
        assert_eq!(positions.len(), 1);
        assert_eq!(positions[0], [1.0, 2.0, 3.0]);
    }

    #[test]
    fn test_parse_node_positions_partial_triplet() {
        let positions = parse_node_positions("1,2;3,4,5");
        assert_eq!(positions.len(), 1);
        assert_eq!(positions[0], [3.0, 4.0, 5.0]);
    }

    fn history_with(frame: Vec<f64>) -> VecDeque<Vec<f64>> {
        let mut h = VecDeque::new();
        h.push_back(frame);
        h
    }

    #[test]
    fn fresh_model_collects_first_matching_frame() {
        let mut field = FieldModel::new(single_link_config()).unwrap();
        assert_eq!(field.status(), CalibrationStatus::Uncalibrated);
        let n = single_link_config().n_subcarriers;
        let history = history_with((0..n).map(|i| 10.0 + i as f64).collect());

        maybe_feed_calibration(&mut field, &history);

        assert_eq!(field.status(), CalibrationStatus::Collecting);
        assert_eq!(field.calibration_frame_count(), 1);

        maybe_feed_calibration(&mut field, &history);
        assert_eq!(field.calibration_frame_count(), 2);
    }

    #[test]
    fn mismatched_frame_is_resampled_and_counted() {
        let mut field = FieldModel::new(single_link_config()).unwrap();
        let history = history_with((0..128).map(|i| i as f64).collect());

        maybe_feed_calibration(&mut field, &history);

        assert_eq!(field.status(), CalibrationStatus::Collecting);
        assert_eq!(field.calibration_frame_count(), 1);
    }

    #[test]
    fn empty_history_does_nothing() {
        let mut field = FieldModel::new(single_link_config()).unwrap();
        maybe_feed_calibration(&mut field, &VecDeque::new());
        assert_eq!(field.status(), CalibrationStatus::Uncalibrated);
        assert_eq!(field.calibration_frame_count(), 0);
    }

    #[test]
    fn resample_linear_identity_and_endpoints() {
        let src: Vec<f64> = (0..56).map(|i| i as f64).collect();
        assert_eq!(resample_linear(&src, 56), src);

        let ramp: Vec<f64> = (0..128).map(|i| i as f64).collect();
        let out = resample_linear(&ramp, 56);
        assert_eq!(out.len(), 56);
        assert!((out[0] - 0.0).abs() < 1e-9);
        assert!((out[55] - 127.0).abs() < 1e-9);
        assert!(out.windows(2).all(|w| w[1] > w[0]), "ramp must stay monotone");

        let up = resample_linear(&[0.0, 10.0], 5);
        assert_eq!(up, vec![0.0, 2.5, 5.0, 7.5, 10.0]);

        assert_eq!(resample_linear(&[], 3), vec![0.0; 3]);
        assert!(resample_linear(&[1.0, 2.0], 0).is_empty());
        assert_eq!(resample_linear(&[4.0], 3), vec![4.0; 3]);
    }
}
