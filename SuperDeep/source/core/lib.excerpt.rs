//! The 3D lyric stage's core, called from C++ (cpp/lyric_stage_native.cpp) through a plain C ABI.
//! No graphics calls here: a world is laid out per song (cheap: positions and timing only), then each
//! frame becomes vertex data the C++ side uploads. Glyph pictures are not part of the world: each frame
//! looks them up in a key -> atlas cell table that fills while the song plays. See scene.rs and frame.rs.

pub mod frame;
pub mod math;
pub mod sdf;

use frame::{AtlasInfo, Cmd, Frame, GlyphV, Mask, ShapeV, Viewport};
use scene::{GlyphIn, LineIn, Scene, WordIn};

#[repr(C)]
pub struct LsSceneParams {
    pub width: f32,
    pub height: f32,
    /// Where the words may stand, in px from the stage's top (the rest is bleed under the fade).
    pub safe_top: f32,
    pub safe_bottom: f32,
    pub atlas_width: i32,
    pub atlas_height: i32,
    pub atlas_cell: i32,
    pub atlas_em_px: f32,
    pub line_count: i32,
    pub word_count: i32,
    pub glyph_count: i32,
    /// The song's beats and bar starts in seconds (0 of either without an analysed grid).
    pub beat_count: i32,
    pub bar_count: i32,
    /// The scene picked in Settings: `scene::MODE_*` (0 smart cuts, 1 the street, 2 pages only, 3 the hall only).
    pub scene_kind: i32,
}

#[repr(C)]
pub struct LsFrameView {
    pub shape_verts: *const ShapeV,
    pub shape_vert_count: i32,
    pub shape_indices: *const u16,
    pub shape_index_count: i32,
    pub glyph_verts: *const GlyphV,
    pub glyph_vert_count: i32,
    pub cmds: *const Cmd,
    pub cmd_count: i32,
    pub high_rate: i32,
}

pub struct Stage { scene: Scene, vp: Viewport, atlas: AtlasInfo }

/// # Safety
/// `stage` must come from `ls_stage_build` and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn ls_stage_free(stage: *mut Stage) {
    if !stage.is_null() { drop(Box::from_raw(stage)); }
}

#[no_mangle]
pub extern "C" fn ls_frame_new() -> *mut Frame { Box::into_raw(Box::default()) }

/// # Safety
/// `frame` must come from `ls_frame_new` and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn ls_frame_free(frame: *mut Frame) {
    if !frame.is_null() { drop(Box::from_raw(frame)); }
}

/// The view stays valid until the next render into this frame. `slots[key]` is the atlas cell of a
/// glyph key, or -1 for one not uploaded yet (it is then simply not drawn).
/// # Safety
/// All pointers must be valid, `slots` holding `slot_count` elements (it may be null when 0); a frame
/// must be rendered by one thread at a time.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ls_frame_render(
    frame: *mut Frame, stage: *const Stage, slots: *const i32, slot_count: i32, t: f32, pulse: f32, fade: f32,
    mask_progress: f32, mask_start: f32, mask_end: f32, mask_left: f32, mask_right: f32,
    out: *mut LsFrameView,
) {
    let (Some(fr), Some(st), Some(view)) = (frame.as_mut(), stage.as_ref(), out.as_mut()) else { return };
    let mask = Mask { progress: mask_progress, start: mask_start, end: mask_end, left: mask_left, right: mask_right };
    let slots: &[i32] = if slots.is_null() || slot_count <= 0 { &[] } else { std::slice::from_raw_parts(slots, slot_count as usize) };
    frame::render(fr, &st.scene, st.vp, st.atlas, slots, t, pulse, fade, &mask);
    write_view(fr, view);
}

/// The live low / mid / high levels (0..1) for the frames that follow: what the spectrum of a long
/// wait shows. Without them it is drawn from the beat grid alone.
/// # Safety
/// `frame` must come from `ls_frame_new`.
#[no_mangle]
pub unsafe extern "C" fn ls_frame_set_bands(frame: *mut Frame, low: f32, mid: f32, high: f32) {
    if let Some(fr) = frame.as_mut() {
        let ok = |x: f32| if x.is_finite() { x.clamp(0.0, 1.0) } else { 0.0 };
        fr.bands = [ok(low), ok(mid), ok(high)];
    }
}

/// The empty stage: black ground and soft edges only.
/// Light theme on or off for the frames rendered after this call.
/// # Safety
/// `frame` must come from `ls_frame_new`; one thread at a time.
#[no_mangle]
pub unsafe extern "C" fn ls_frame_set_invert(frame: *mut Frame, on: i32) {
    if let Some(fr) = frame.as_mut() { fr.invert = on != 0; }
}

/// # Safety
/// All pointers must be valid; a frame must be rendered by one thread at a time.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ls_frame_blank(
    frame: *mut Frame, width: f32, height: f32,
    mask_progress: f32, mask_start: f32, mask_end: f32, mask_left: f32, mask_right: f32,
    out: *mut LsFrameView,
) {
    let (Some(fr), Some(view)) = (frame.as_mut(), out.as_mut()) else { return };
    let mask = Mask { progress: mask_progress, start: mask_start, end: mask_end, left: mask_left, right: mask_right };
    frame::blank(fr, width.max(1.0), height.max(1.0), &mask);
    write_view(fr, view);
}

fn write_view(fr: &Frame, view: &mut LsFrameView) {
    *view = LsFrameView {
        shape_verts: fr.sv.as_ptr(),
        shape_vert_count: fr.sv.len() as i32,
        shape_indices: fr.si.as_ptr(),
        shape_index_count: fr.si.len() as i32,
        glyph_verts: fr.gv.as_ptr(),
        glyph_vert_count: fr.gv.len() as i32,
        cmds: fr.cmds.as_ptr(),
        cmd_count: fr.cmds.len() as i32,
        high_rate: fr.high_rate as i32,
    };
}

/// # Safety
/// `pixels` must hold `stride * height` bytes.
#[no_mangle]
pub unsafe extern "C" fn ls_sdf_build(pixels: *mut u8, width: i32, height: i32, stride: i32, cell: i32, spread: f32) {
    if pixels.is_null() || width <= 0 || height <= 0 || stride < width || cell <= 0 { return; }
    let bytes = std::slice::from_raw_parts_mut(pixels, stride as usize * height as usize);
    sdf::build(bytes, width as usize, height as usize, stride as usize, cell as usize, spread);
}
