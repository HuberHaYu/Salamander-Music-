//! Turns the atlas's antialiased glyph masks into signed distance fields, once per song. One texture
//! then stays sharp at any size and perspective and gives the fill, the hollow outline and the glow,
//! so nothing is rasterised per frame.
//!
//! Exact Euclidean transform (Felzenszwalb & Huttenlocher), seeded from the mask's coverage so the
//! edge keeps its sub-pixel position.

const INF: f32 = 1e20;

/// Rewrites `pixels` (one byte per pixel, `stride` per row) cell by cell.
/// 0.5 (128) is the glyph's edge; 0 and 255 are `spread` pixels out and in.
pub fn build(pixels: &mut [u8], width: usize, height: usize, stride: usize, cell: usize, spread: f32) {
    if cell == 0 || width < cell || height < cell || stride < width { return; }
    if pixels.len() < stride * height { return; }
    let mut outer = vec![0f32; cell * cell];
    let mut inner = vec![0f32; cell * cell];
    let mut f = vec![0f32; cell];
    let mut d = vec![0f32; cell];
    let mut v = vec![0usize; cell];
    let mut z = vec![0f32; cell + 1];
    for cy in 0..height / cell {
        for cx in 0..width / cell {
            let origin = cy * cell * stride + cx * cell;
            let mut any = false;
            for y in 0..cell {
                let row = &pixels[origin + y * stride..origin + y * stride + cell];
                for x in 0..cell {
                    let a = row[x] as f32 / 255.0;
                    let i = y * cell + x;
                    if a >= 1.0 {
                        outer[i] = 0.0;
                        inner[i] = INF;
                    } else if a <= 0.0 {
                        outer[i] = INF;
                        inner[i] = 0.0;
                    } else {
                        let o = (0.5 - a).max(0.0);
                        let n = (a - 0.5).max(0.0);
                        outer[i] = o * o;
                        inner[i] = n * n;
                    }
                    any |= a > 0.0;
                }
            }
            if !any { continue; }
            edt(&mut outer, cell, &mut f, &mut d, &mut v, &mut z);
            edt(&mut inner, cell, &mut f, &mut d, &mut v, &mut z);
            for y in 0..cell {
                let row = &mut pixels[origin + y * stride..origin + y * stride + cell];
                for x in 0..cell {
                    let i = y * cell + x;
                    let dist = outer[i].sqrt() - inner[i].sqrt();
                    let value = 0.5 - dist / (2.0 * spread);
                    row[x] = (value.clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
                }
            }
        }
    }
}

fn edt(grid: &mut [f32], n: usize, f: &mut [f32], d: &mut [f32], v: &mut [usize], z: &mut [f32]) {
    for x in 0..n {
        for y in 0..n { f[y] = grid[y * n + x]; }
        edt1d(f, d, v, z, n);
        for y in 0..n { grid[y * n + x] = d[y]; }
    }
    for y in 0..n {
        f[..n].copy_from_slice(&grid[y * n..y * n + n]);
        edt1d(f, d, v, z, n);
        grid[y * n..y * n + n].copy_from_slice(&d[..n]);
    }
}

fn edt1d(f: &[f32], d: &mut [f32], v: &mut [usize], z: &mut [f32], n: usize) {
    let mut k = 0usize;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    for q in 1..n {
        let fq = f[q] + (q * q) as f32;
        let mut s;
        loop {
            let p = v[k];
            s = (fq - f[p] - (p * p) as f32) / (2.0 * (q as f32 - p as f32));
            if s <= z[k] && k > 0 { k -= 1; } else { break; }
        }
        if s <= z[k] { continue; }
        k += 1;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }
    k = 0;
    for q in 0..n {
        while z[k + 1] < q as f32 { k += 1; }
        let p = v[k];
        let delta = q as f32 - p as f32;
        d[q] = delta * delta + f[p];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edge_is_half_and_far_is_clamped() {
        let cell = 32;
        let mut px = vec![0u8; cell * cell];
        for y in 10..22 { for x in 10..22 { px[y * cell + x] = 255; } }
        build(&mut px, cell, cell, cell, cell, 4.0);
        assert_eq!(px[0], 0, "far outside saturates");
        assert!(px[16 * cell + 16] == 255, "deep inside saturates");
        let edge = px[16 * cell + 10] as i32;
        assert!((edge - 128).abs() < 40, "edge sits near one half, got {edge}");
        // Falls monotonically leaving the glyph.
        let row: Vec<u8> = (2..16).map(|x| px[16 * cell + x]).collect();
        assert!(row.windows(2).all(|w| w[0] <= w[1]), "{row:?}");
    }

    #[test]
    fn empty_cell_stays_empty() {
        let mut px = vec![0u8; 64 * 64];
        build(&mut px, 64, 64, 64, 32, 4.0);
        assert!(px.iter().all(|&b| b == 0));
    }
}
