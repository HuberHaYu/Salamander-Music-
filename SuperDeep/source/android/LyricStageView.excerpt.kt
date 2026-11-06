package com.lab.hv

import android.content.Context
import android.graphics.Outline
import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.ViewOutlineProvider
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * The 3D lyric stage where the Now Playing MV goes. Its picture is drawn on a native thread into this
 * TextureView's surface, soft edges included, so here it costs one textured quad and needs no layer.
 * Laid out once at the full-size frame: the player's expand and collapse only transform the view, so
 * its buffer is never reallocated while moving.
 *
 * Nothing is rendered ahead for a whole song. New lyrics get their world at once (layout only, a few
 * milliseconds); the glyphs the next lines need are drawn before it is shown, and the rest are drawn
 * while the song plays, nearest to the playhead first, into an atlas kept across songs. Another song's
 * lyrics clear the stage the moment they are known, so it never shows words that are not playing.
 */
class LyricStageView(context: Context) : TextureView(context), TextureView.SurfaceTextureListener, AudioBandPulse.Bars {

    companion object {
        /** Smart cuts: verses on 2D pages, choruses and the title in the hall, white-field cuts between (智能转场). */
        const val SCENE_SMART = 0
        /** The whole song in a city without cars, each line a shot of its own (城区). */
        const val SCENE_CITY = 1
        /** The whole song on 2D pages (平面). */
        const val SCENE_FLAT = 2
        /** The whole song in the hall, lines hanging as vertical scrolls (空间幕布). */
        const val SCENE_HALL = 3
        /** The introduction's demo loop: city, then a poster, then the city again. */
        const val SCENE_DEMO_CITY = 5
        /** The introduction's demo loop in the hall. */
        const val SCENE_DEMO_HALL = 6

    }

    /** The full-size frame, in the parent's coordinates. */
    fun setFrame(left: Int, top: Int, right: Int, bottom: Int, safeTop: Float) {
        val width = right - left
        val height = bottom - top
        if (width <= 0 || height <= 0) return
        if (isLayoutRequested || measuredWidth != width || measuredHeight != height) {
            measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY))
        }
        if (getLeft() != left || getTop() != top || getRight() != right || getBottom() != bottom) {
            layout(left, top, right, bottom)
        }
        safeTopPx = safeTop
        if (lines.isNotEmpty() && (builtWidth != width || builtHeight != height || abs(builtSafeTop - safeTop) > 0.5f)) {
            // The same lyrics at a new size: swapped in place, its glyphs already in the atlas.
            requestBuild()
        }
    }

    /** Where the view shows right now, as a transform of the full-size frame. */
    fun setMorph(left: Float, top: Float, right: Float, bottom: Float, radius: Float) {
        val width = width.coerceAtLeast(1)
        val height = height.coerceAtLeast(1)
        translationX = left - getLeft()
        translationY = top - getTop()
        scaleX = (right - left) / width
        scaleY = (bottom - top) / height
        val wantClip = radius > 0.5f
        if (cornerRadius != radius) {
            cornerRadius = radius
            if (wantClip) invalidateOutline()
        }
        if (clipToOutline != wantClip) clipToOutline = wantClip
    }

    fun setEdges(fadeStart: Float, fadeEnd: Float, leftPx: Float, rightPx: Float) {
        wordsBottomRatio = Float.NaN
        applyMask(1f, fadeStart, fadeEnd, leftPx, rightPx)
    }

    /** Whether frames may be drawn: on screen, in the foreground, the player not moving. */
    fun setActive(active: Boolean) {
        if (activeWanted == active || handle == 0L) return
        activeWanted = active
        LyricStageNative.nativeSetActive(handle, active)
        syncPulse()
    }

    fun setClock(positionMs: Long, playing: Boolean, speed: Float, force: Boolean) {
        if (handle == 0L) return
        LyricStageNative.nativeSetClock(handle, positionMs, playing, speed, force)
        clockPositionMs = positionMs
        clockUptimeMs = SystemClock.uptimeMillis()
        clockPlaying = playing
        clockSpeed = speed
        // A full atlas: glyphs further on are drawn when the song gets near them, or after a seek.
        if (refillWaiting && (force || positionMs >= refillAtMs)) {
            refillWaiting = false
            val token = generation
            builder.execute { stream(token) }
        }
    }

    private fun playheadSeconds(): Float {
        val base = clockPositionMs
        val elapsed = if (clockPlaying) (SystemClock.uptimeMillis() - clockUptimeMs) * clockSpeed else 0f
        return (base + elapsed) / 1000f
    }

    override fun barsHost(): View = this

    private fun requestBuild() {
        val h = handle
        if (h == 0L || released) return
        val width = measuredWidth
        val height = measuredHeight
        if (width <= 0 || height <= 0) return
        builtWidth = width
        builtHeight = height
        builtSafeTop = safeTopPx
        val token = ++generation
        val snapshotLines = lines
        val snapshotTitle = title
        val gridOk = beatsTrackKey == trackKey
        val snapshotBeats = if (gridOk) beats else null
        val snapshotBars = if (gridOk) bars else null
        val safeTop = safeTopPx
        val fadeIn = fadeNextScene
        val kind = sceneKind
        fadeNextScene = false
        refillWaiting = false
        // Words stand above where the bottom fade has eaten into them.
        val wordsRatio = if (wordsBottomRatio.isNaN()) maskStart + (maskEnd - maskStart) * 0.42f else wordsBottomRatio
        val safeBottom = height * wordsRatio.coerceIn(0.5f, 0.86f)
        builder.execute {
            if (token != generation) return@execute
            setPriority(Process.THREAD_PRIORITY_DISPLAY)
            val installed = try {
                build(h, token, fadeIn, snapshotLines, snapshotTitle, snapshotBeats, snapshotBars, width, height, safeTop, safeBottom, kind)
            } catch (_: RuntimeException) {
                false
            } catch (_: OutOfMemoryError) {
                false
            }
            if (!installed && token == generation) {
                song = null
                LyricStageNative.nativeClearScene(h)
            }
            mainHandler.post {
                if (token == generation && !released) sceneListener?.onSceneAvailable(installed)
            }
            if (installed) stream(token)
        }
    }

    /** Builder thread: the world now, with the glyphs of the lines about to show. */
    private fun build(
        h: Long, token: Long, fadeIn: Boolean, lines: List<LyricStageLine>, title: String?,
        beats: FloatArray?, bars: FloatArray?, width: Int, height: Int, safeTop: Float, safeBottom: Float,
        kind: Int,
    ): Boolean {
        glyphs.syncFont()
        val prepared = LyricStageScene.prepare(lines, title, glyphs::advance) ?: return false
        val keySlots = IntArray(prepared.keys.size) { glyphs.slot(prepared.keys[it]) }
        for (cell in keySlots) if (cell >= 0) glyphs.touch(cell)
        val next = Song(token, prepared, keySlots)
        val cur = prepared.currentLine(playheadSeconds())
        scoreKeys(next, cur)
        val shown = song
        val upload = drawMissing(next, NEAR_LINES_SCORE, Int.MAX_VALUE) { text -> shown?.keyIndex?.containsKey(text) == true }
        if (token != generation) {
            // Superseded: the cells are drawn and the cache counts on them, so they still go up.
            if (upload != null) LyricStageNative.nativeUpdateGlyphs(h, 0L, null, upload.first, upload.second)
            return false
        }
        // The cells go up with the world even if Rust turns it down.
        val ok = LyricStageNative.nativeSetScene(
            h, token, fadeIn, width.toFloat(), height.toFloat(), safeTop, safeBottom, LyricStageAtlas.EM_PX,
            prepared.lineWordStart, prepared.wordGlyphStart, prepared.lineTimes, prepared.lineFlags,
            prepared.glyphData, keySlots.copyOf(), upload?.first, upload?.second, beats, bars, kind,
        )
        if (ok) song = next
        return ok
    }

    /**
     * Builder thread: draws the song's remaining glyphs in small batches, nearest to the playhead first
     * (re-read each batch, so a seek is followed at once), at background priority. Ends when every
     * glyph is in the atlas, or waits for the song to move on when the atlas is full of nearer ones.
     */
    private fun stream(token: Long) {
        setPriority(Process.THREAD_PRIORITY_BACKGROUND)
        while (token == generation && !released) {
            val s = song ?: return
            if (s.serial != token) return
            val prepared = s.prepared
            val cur = prepared.currentLine(playheadSeconds())
            scoreKeys(s, cur)
            val upload = drawMissing(s, Int.MAX_VALUE, STREAM_BATCH) { false }
            if (upload == null) {
                if (s.keySlots.any { it < 0 }) {
                    // Full: try again once the playhead reaches the next lines.
                    val nextLine = min(prepared.lineT0.size - 1, cur + 2)
                    refillAtMs = (prepared.lineT0[nextLine] * 1000f).toLong()
                    refillWaiting = true
                }
                return
            }
            // Sent even if superseded meanwhile: then only the cells apply, not the old key table.
            val h = handle
            if (h == 0L) return
            LyricStageNative.nativeUpdateGlyphs(h, s.serial, s.keySlots.copyOf(), upload.first, upload.second)
        }
    }

    /** A world for the song playing now is urgent; the rest of its glyphs are not. */
    private fun setPriority(priority: Int) {
        try {
            Process.setThreadPriority(priority)
        } catch (_: RuntimeException) {
        }
    }

    /** How soon each key is needed: lines ahead count one per line, lines behind count triple. */
    private fun scoreKeys(s: Song, cur: Int) {
        s.keyScore.fill(Int.MAX_VALUE)
        val lineKeys = s.prepared.lineKeys
        for (li in lineKeys.indices) {
            val score = if (li >= cur) li - cur else (cur - li) * 3 + 1
            for (k in lineKeys[li]) if (score < s.keyScore[k]) s.keyScore[k] = score
        }
    }

    /** Stops the renderer and its threads; the view is not used again. Call after removing it. */
    fun release() {
        if (released) return
        released = true
        generation++
        sceneListener = null
        setActive(false)
        val h = handle
        handle = 0L
        if (pulseRegistered) {
            pulseRegistered = false
            AudioBandPulse.with(context).remove(this)
        }
        // Behind any build still running, which holds the same handle.
        builder.execute {
            if (h != 0L) LyricStageNative.nativeDestroy(h)
        }
        builder.shutdown()
    }

    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
        val next = Surface(texture)
        surface = next
        if (handle != 0L) {
            LyricStageNative.nativeSetSurface(handle, next)
            LyricStageNative.nativeSetMask(handle, maskProgress, maskStart, maskEnd, maskLeft, maskRight)
        }
    }

    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
        // The renderer reads the surface's size on its next frame.
    }

    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
        if (handle != 0L) LyricStageNative.nativeSetSurface(handle, null)
        surface?.release()
        surface = null
        return true
    }

    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
}
