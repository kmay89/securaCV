//! The OS random number generator, and nothing else.
//!
//! Every nonce, handle, key and household id the checker draws comes from
//! here. There is no seeded or userspace generator anywhere in the crate, and
//! a failure of the OS source is an error: nothing falls back to a weaker
//! source.

use crate::{Error, Result};
use rand::rngs::SysRng;
use rand::TryRng;

/// Fill `buf` from the OS random number generator.
pub fn fill(buf: &mut [u8]) -> Result<()> {
    SysRng.try_fill_bytes(buf).map_err(|_| Error::Rng)
}

/// An array of `N` bytes from the OS random number generator.
pub fn array<const N: usize>() -> Result<[u8; N]> {
    let mut out = [0u8; N];
    fill(&mut out)?;
    Ok(out)
}

/// A uniformly distributed index in `0..n` (`n > 0`), by rejection sampling.
pub fn index(n: usize) -> Result<usize> {
    if n == 0 {
        return Err(Error::Invalid("index range is empty"));
    }
    let n = n as u64;
    // Largest multiple of n that fits; values at or above it are redrawn so
    // the result carries no modulo bias.
    let zone = u64::MAX - (u64::MAX % n);
    loop {
        let v = u64::from_le_bytes(array::<8>()?);
        if v < zone {
            return Ok((v % n) as usize);
        }
    }
}

/// Shuffle a slice in place (Fisher-Yates) with OS randomness.
pub fn shuffle<T>(items: &mut [T]) -> Result<()> {
    for i in (1..items.len()).rev() {
        let j = index(i + 1)?;
        items.swap(i, j);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn draws_differ() {
        let a: [u8; 32] = array().unwrap();
        let b: [u8; 32] = array().unwrap();
        assert_ne!(a, b);
    }

    #[test]
    fn index_in_range_and_shuffle_is_a_permutation() {
        for n in 1..20 {
            assert!(index(n).unwrap() < n);
        }
        assert!(index(0).is_err());
        let mut v: Vec<u32> = (0..32).collect();
        shuffle(&mut v).unwrap();
        let mut sorted = v.clone();
        sorted.sort();
        assert_eq!(sorted, (0..32).collect::<Vec<_>>());
    }
}
