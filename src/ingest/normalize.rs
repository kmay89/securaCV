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
                .ok_or_else(|| anyhow!("RGB frame dimensions overflow"))?
                as usize;
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
        let row_offset = j * w;
        let uv_row = y_plane + (j / 2) * w;
        for i in (0..w).step_by(2) {
            let uv_index = uv_row + i;
            let u = pixels[uv_index] as i32 - 128;
            let v = pixels[uv_index + 1] as i32 - 128;

            let r_add = (1436 * v) / 1024;
            let g_add = (-352 * u - 731 * v) / 1024;
            let b_add = (1815 * u) / 1024;

            let y1 = pixels[row_offset + i] as i32;
            let offset1 = (row_offset + i) * 3;
            rgb[offset1] = clamp_to_u8_i32(y1 + r_add);
            rgb[offset1 + 1] = clamp_to_u8_i32(y1 + g_add);
            rgb[offset1 + 2] = clamp_to_u8_i32(y1 + b_add);

            if i + 1 < w {
                let y2 = pixels[row_offset + i + 1] as i32;
                let offset2 = (row_offset + i + 1) * 3;
                rgb[offset2] = clamp_to_u8_i32(y2 + r_add);
                rgb[offset2 + 1] = clamp_to_u8_i32(y2 + g_add);
                rgb[offset2 + 2] = clamp_to_u8_i32(y2 + b_add);
            }
        }
    }

    Ok(rgb)
}

fn clamp_to_u8_i32(value: i32) -> u8 {
    value.clamp(0, 255) as u8
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
