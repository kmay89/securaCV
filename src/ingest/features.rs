use sha2::{Digest, Sha256};

/// Compute non-invertible feature hash from pixels.
///
/// This hash is used for correlation tokens and MUST NOT:
/// - Be invertible back to pixel data
/// - Contain identity-bearing information (faces, plates, etc.)
/// - Be stable across long time windows
///
/// In production, this would derive from:
/// - Color histograms (coarse)
/// - Motion vector magnitudes (not directions)
/// - Texture energy (not structure)
pub(crate) fn compute_features_hash(pixels: &[u8], frame_count: u64) -> [u8; 32] {
    let mut hasher = Sha256::new();

    // Coarse color histogram (very lossy)
    let mut histogram = [0u32; 8]; // 8 bins
    for &p in pixels.iter().step_by(300) {
        // Sample every 100th pixel
        histogram[(p / 32) as usize] += 1;
    }
    for count in &histogram {
        hasher.update(count.to_le_bytes());
    }

    // Add frame-local noise to prevent cross-frame stability
    hasher.update(frame_count.to_le_bytes());
    hasher.update(rand::random::<u64>().to_le_bytes());

    hasher.finalize().into()
}
