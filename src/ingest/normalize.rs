use anyhow::{anyhow, Result};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PixelFormat {
    Rgb24,
    Nv12,
}

pub(crate) fn normalize_to_rgb(
    pixels: &[u8],
    width: u32,
    height: u32,
    format: PixelFormat,
) -> Result<Vec<u8>> {
    match format {
        PixelFormat::Rgb24 => {
            let expected = width
                .checked_mul(height)
                .and_then(|v| v.checked_mul(3))
                .ok_or_else(|| anyhow!("RGB frame dimensions overflow"))? as usize;
            if pixels.len() != expected {
                return Err(anyhow!(
                    "RGB frame length mismatch: expected {}, got {}",
                    expected,
                    pixels.len()
                ));
            }
            Ok(pixels.to_vec())
        }
        PixelFormat::Nv12 => nv12_to_rgb(pixels, width, height),
    }
}

fn nv12_to_rgb(pixels: &[u8], width: u32, height: u32) -> Result<Vec<u8>> {
    let w = width as usize;
    let h = height as usize;
    let y_plane = w
        .checked_mul(h)
        .ok_or_else(|| anyhow!("NV12 frame dimensions overflow"))?;
    let expected = y_plane
        .checked_add(y_plane / 2)
        .ok_or_else(|| anyhow!("NV12 frame dimensions overflow"))?;
    if pixels.len() != expected {
        return Err(anyhow!(
            "NV12 frame length mismatch: expected {}, got {}",
            expected,
            pixels.len()
        ));
    }

    let mut rgb = vec![0u8; y_plane * 3];
    for j in 0..h {
        for i in 0..w {
            let y = pixels[j * w + i] as f32;
            let uv_index = y_plane + (j / 2) * w + (i / 2) * 2;
            let u = pixels[uv_index] as f32 - 128.0;
            let v = pixels[uv_index + 1] as f32 - 128.0;

            let r = y + 1.402_f32 * v;
            let g = y - 0.344_136_f32 * u - 0.714_136_f32 * v;
            let b = y + 1.772_f32 * u;

            let offset = (j * w + i) * 3;
            rgb[offset] = clamp_to_u8(r);
            rgb[offset + 1] = clamp_to_u8(g);
            rgb[offset + 2] = clamp_to_u8(b);
        }
    }

    Ok(rgb)
}

fn clamp_to_u8(value: f32) -> u8 {
    value.round().clamp(0.0, 255.0) as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nv12_conversion_produces_gray() -> Result<()> {
        let width = 2;
        let height = 2;
        let y_plane = vec![128u8; 4];
        let uv_plane = vec![128u8; 2];
        let nv12 = [y_plane, uv_plane].concat();

        let rgb = normalize_to_rgb(&nv12, width, height, PixelFormat::Nv12)?;
        assert_eq!(rgb, vec![128u8; 12]);

        Ok(())
    }

    #[test]
    fn rgb_pass_through_validates_length() -> Result<()> {
        let pixels = vec![1u8; 9];
        let rgb = normalize_to_rgb(&pixels, 1, 3, PixelFormat::Rgb24)?;
        assert_eq!(rgb, pixels);
        Ok(())
    }
}
