//! One frame: projects the part of the world near the camera into vertex data for a few draw calls.
//! Lines and solid shapes get their antialiasing and soft glow from extra geometry with a falling
//! vertex alpha, and glyphs from the distance field, so the picture needs no MSAA and no offscreen
//! or full-screen pass. Far parts of a long edge fade per vertex, without being subdivided much.

use crate::math::*;
use crate::scene::*;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShapeV { pub x: f32, pub y: f32, pub lum: f32, pub a: f32 }

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GlyphV {
    pub x: f32, pub y: f32, pub w: f32,
    pub u: f32, pub v: f32, pub lx: f32,
    pub fill: f32, pub stroke: f32, pub glow: f32, pub sweep: f32,
    /// 0 sharp … 1 fully out of focus: widens the distance field's edge (a Gaussian-like defocus).
    pub blur: f32,
}

pub const CMD_SHAPES: u32 = 0;
pub const CMD_GLYPHS: u32 = 1;
pub const CMD_MASK: u32 = 2;
/// One full-stage quad that inverts the picture drawn so far (light theme), before the mask.
pub const CMD_INVERT: u32 = 3;
/// Glyph quads that darken what is under them (dst × (1 − coverage)): the street's words knock out
/// lit windows behind them before they are drawn.
pub const CMD_KNOCK: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Cmd { pub kind: u32, pub first: u32, pub count: u32 }

#[derive(Clone, Copy)]
pub struct Viewport { pub w: f32, pub h: f32, pub cx: f32, pub cy: f32, pub f: f32, pub s: f32 }

#[derive(Clone, Copy)]
pub struct AtlasInfo { pub cols: i32, pub cell_u: f32, pub cell_v: f32, pub half_em: f32 }

#[derive(Clone, Copy, Default)]
pub struct Mask { pub progress: f32, pub start: f32, pub end: f32, pub left: f32, pub right: f32 }

pub(crate) const NEAR: f32 = 0.6;
const MAX_SHAPE_VERTS: usize = 64_000;
pub(crate) const MAX_QUADS: usize = 16_000;

#[derive(Default)]
pub struct Frame {
    pub sv: Vec<ShapeV>,
    pub si: Vec<u16>,
    pub gv: Vec<GlyphV>,
    pub cmds: Vec<Cmd>,
    pub high_rate: bool,
    /// The live low / mid / high levels (0..1) the spectrum shows; zero when none are given.
    pub bands: [f32; 3],
    /// Light theme: the finished picture is inverted (black lines on a white ground).
    pub invert: bool,
}

fn draw_mask(cx: &mut Ctx, m: &Mask) {
    let (w, h) = (cx.vp.w, cx.vp.h);
    let want_bottom = m.progress > 0.001;
    let want_sides = m.left > 0.5 || m.right > 0.5;
    if !want_bottom && !want_sides { return; }
    cx.begin(CMD_MASK);
    if want_bottom {
        const ROWS: i32 = 28;
        let y_start = m.start * h;
        let mut prev_l = cx.vert(0.0, y_start, 0.0, 1.0);
        let mut prev_r = cx.vert(w, y_start, 0.0, 1.0);
        for i in 1..=ROWS {
            let u = i as f32 / ROWS as f32;
            let ratio = m.start + (1.0 - m.start) * u;
            let tt = clamp01((ratio - m.start) / (m.end - m.start).max(0.001));
            let full = 1.0 - tt * tt * (3.0 - 2.0 * tt);
            let mut a = 1.0 - m.progress * (1.0 - full);
            if m.progress >= 0.999 && ratio >= m.end { a = 0.0; }
            let l = cx.vert(0.0, ratio * h, 0.0, a);
            let r = cx.vert(w, ratio * h, 0.0, a);
            cx.quad_idx(prev_l, prev_r, r, l);
            prev_l = l;
            prev_r = r;
        }
    }
    let side = |cx: &mut Ctx, x_edge: f32, x_in: f32| {
        const COLS: i32 = 8;
        let mut p0 = cx.vert(x_edge, 0.0, 0.0, 0.0);
        let mut p1 = cx.vert(x_edge, h, 0.0, 0.0);
        for i in 1..=COLS {
            let u = i as f32 / COLS as f32;
            let a = u * u * (3.0 - 2.0 * u);
            let x = x_edge + (x_in - x_edge) * u;
            let q0 = cx.vert(x, 0.0, 0.0, a);
            let q1 = cx.vert(x, h, 0.0, a);
            cx.quad_idx(p0, q0, q1, p1);
            p0 = q0;
            p1 = q1;
        }
    };
    if m.left > 0.5 { side(cx, 0.0, m.left); }
    if m.right > 0.5 { side(cx, w, w - m.right); }
}

/// The fade from black and the soft edges, over everything.
fn finish(cx: &mut Ctx, vp: Viewport, fade: f32, mask: &Mask) {
    if fade < 0.999 {
        cx.begin(CMD_SHAPES);
        let (w, h) = (vp.w, vp.h);
        let a = 1.0 - clamp01(fade);
        let v0 = cx.vert(0.0, 0.0, 0.0, a);
        let v1 = cx.vert(w, 0.0, 0.0, a);
        let v2 = cx.vert(w, h, 0.0, a);
        let v3 = cx.vert(0.0, h, 0.0, a);
        cx.quad_idx(v0, v1, v2, v3);
    }
    invert(cx, vp.w, vp.h);
    draw_mask(cx, mask);
}

/// Light theme: everything drawn so far turns over, white ground and black lines; the soft edges
/// come after, so they still fade the picture out the same way.
fn invert(cx: &mut Ctx, w: f32, h: f32) {
    if !cx.out.invert { return; }
    cx.begin(CMD_INVERT);
    let v0 = cx.vert(0.0, 0.0, 1.0, 1.0);
    let v1 = cx.vert(w, 0.0, 1.0, 1.0);
    let v2 = cx.vert(w, h, 1.0, 1.0);
    let v3 = cx.vert(0.0, h, 1.0, 1.0);
    cx.quad_idx(v0, v1, v2, v3);
}

fn out_rate(out: &mut Frame, high: bool) { out.high_rate = high; }

/// The stage with no world in it (between songs): its black ground under the same soft edges, so the
/// last song's words never stay frozen on screen while the next one is prepared.
pub fn blank(out: &mut Frame, w: f32, h: f32, mask: &Mask) {
    out.sv.clear();
    out.si.clear();
    out.gv.clear();
    out.cmds.clear();
    let vp = Viewport { w, h, cx: w * 0.5, cy: h * 0.5, f: 1.0, s: 1.0 };
    let atlas = AtlasInfo { cols: 1, cell_u: 1.0, cell_v: 1.0, half_em: 0.5 };
    let mut cx = Ctx { out, vp, atlas, slots: &[], far: 64.0, e: [0.0; 3], r: [1.0, 0.0, 0.0], u: [0.0, 1.0, 0.0], f: [0.0, 0.0, -1.0], fog_off: 0.0, flat: 0.0, blur: 0.0 };
    invert(&mut cx, w, h);
    draw_mask(&mut cx, mask);
    out.high_rate = false;
    out.cmds.retain(|c| c.count > 0);
}
