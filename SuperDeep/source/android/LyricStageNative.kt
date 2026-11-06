package com.lab.hv

import android.view.Surface
import java.nio.ByteBuffer

/** The native side of the 3D lyric stage: cpp/lyric_stage_native.cpp over rust/lyricstage. */
internal object LyricStageNative {
    /** False when a library is missing or does not load: the stage is then simply never offered. */
    @JvmField
    val available: Boolean = try {
        System.loadLibrary("salam_native")
        System.loadLibrary("lyric_stage_native")
        true
    } catch (_: Throwable) {
        false
    }

    /** One renderer with its own thread and an atlas texture of the given size, kept for its life. */
    @JvmStatic external fun nativeCreate(atlasWidth: Int, atlasHeight: Int, cell: Int): Long
    @JvmStatic external fun nativeDestroy(handle: Long)
    /** Returns once the renderer no longer uses the previous surface. */
    @JvmStatic external fun nativeSetSurface(handle: Long, surface: Surface?)
    @JvmStatic external fun nativeSetActive(handle: Long, active: Boolean)
    @JvmStatic external fun nativeSetClock(handle: Long, positionMs: Long, playing: Boolean, speed: Float, force: Boolean)
    @JvmStatic external fun nativeSetPulse(handle: Long, pulse: Float)
    /** The live low / mid / high levels (0–1) the stage's spectrum shows during long waits. */
    @JvmStatic external fun nativeSetBands(handle: Long, low: Float, mid: Float, high: Float)
    /** Light theme: the whole picture inverted, black lines on a white ground. */
    @JvmStatic external fun nativeSetInvert(handle: Long, invert: Boolean)
    @JvmStatic external fun nativeSetMask(handle: Long, progress: Float, start: Float, end: Float, left: Float, right: Float)
    @JvmStatic external fun nativeBuildSdf(pixels: ByteBuffer, width: Int, height: Int, stride: Int, cell: Int, spread: Float)
    /**
     * Lays out a world and hands it over with its key -> atlas cell table ([keySlots], -1 for a glyph
     * not drawn yet) and any cells drawn for it ([cellSlots], CELL * CELL bytes each in [cellPixels]).
     * [fadeIn]: a new world rises from black; false swaps in place (the same lyrics at a new size).
     */
    @JvmStatic external fun nativeSetScene(
        handle: Long,
        serial: Long,
        fadeIn: Boolean,
        width: Float,
        height: Float,
        safeTop: Float,
        safeBottom: Float,
        atlasEmPx: Float,
        lineWordStart: IntArray,
        wordGlyphStart: IntArray,
        lineTimes: FloatArray,
        lineFlags: IntArray,
        glyphData: FloatArray,
        keySlots: IntArray,
        cellSlots: IntArray?,
        cellPixels: ByteBuffer?,
        /** The song's beats and bar starts in seconds of stage time; null without an analysed grid. */
        beats: FloatArray?,
        bars: FloatArray?,
        /** Which world the 3D lines live in: 0 the hall (竖卷), 1 the street (长街). */
        sceneKind: Int,
    ): Boolean
    /** More cells drawn while the song plays, with world [serial]'s new key table (ignored for any other world). */
    @JvmStatic external fun nativeUpdateGlyphs(handle: Long, serial: Long, keySlots: IntArray?, cellSlots: IntArray?, cellPixels: ByteBuffer?)
    @JvmStatic external fun nativeClearScene(handle: Long)
}
