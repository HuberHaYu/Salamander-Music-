// The 3D lyric stage's renderer: its own thread and OpenGL ES 3 context, drawing into the TextureView
// that stands where the Now Playing MV goes. The scene, camera and vertex data all come from Rust
// (rust/lyricstage, in libsalam_native.so); this file owns only the window, the clock and a handful
// of draw calls per frame. Nothing here touches the UI thread or HWUI's RenderThread.
//
// Glyphs: one atlas texture lives as long as the renderer, shared by every song. Kotlin rasterises
// glyphs into its cells while the song plays, nearest to the playhead first, and each upload carries
// the song's key -> cell table, so a frame only ever draws glyphs whose picture is in the texture.
// A new song's world is laid out at once (positions and timing only) and is never held back by text.
//
// Power: paced by the display's vsync and capped at 60 fps (about 30 while no word or camera is moving);
// while paused it draws once and then sleeps in the looper, with no timers. No MSAA, no depth buffer
// and no offscreen or full-screen pass: antialiasing and glow are in the geometry and the glyph field.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#define LOG_TAG "SalamLyricStage"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---- The Rust side (rust/lyricstage/src/lib.rs) ----

struct RStage;
struct RFrame;

struct RSceneParams {
    float width, height, safe_top, safe_bottom;
    int32_t atlas_width, atlas_height, atlas_cell;
    float atlas_em_px;
    int32_t line_count, word_count, glyph_count;
    int32_t beat_count, bar_count;
    int32_t scene_kind;
};

struct RCmd { uint32_t kind, first, count; };

struct RFrameView {
    const void* shape_verts;
    int32_t shape_vert_count;
    const uint16_t* shape_indices;
    int32_t shape_index_count;
    const void* glyph_verts;
    int32_t glyph_vert_count;
    const RCmd* cmds;
    int32_t cmd_count;
    int32_t high_rate;
};

using StageBuildFn = RStage* (*)(const RSceneParams*, const int32_t*, const int32_t*, const float*,

int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

constexpr int kMaxQuads = 16000;
constexpr int64_t kFrame60Ns = 16000000LL;   // A touch under 16.7 ms, to stay on 60 at a 120 Hz vsync.
constexpr int64_t kFrame30Ns = 32000000LL;
constexpr int64_t kFadeInNs = 450000000LL;
/// A frame shows about one vsync after it is drawn.
constexpr int64_t kShowLeadNs = 16000000LL;
// The clock follows the player exactly as the scrolling lyrics do (NowPlayingLyricsTickerView), so
// both always show the same moment: a report within kClockSnapMs pulls the running clock a share of
// the way, at most kClockMaxStepMs per report; anything farther, a seek, a pause or a rate change snaps.
constexpr double kClockSnapMs = 420.0;
constexpr double kClockCorrection = 0.18;
constexpr double kClockMaxStepMs = 12.0;

/// One change from the UI side, applied on the render thread in the order it was sent.
struct Op {
    enum Kind { kScene, kClear, kGlyphs } kind = kGlyphs;
    RStage* stage = nullptr;      // kScene: owned until applied.
    uint64_t serial = 0;          // kScene / kGlyphs: which world the key table belongs to.
    bool fadeIn = true;           // kScene: a new world rises from black; a re-layout of the same does not.
    float stageWidth = 1.0f;
    float stageHeight = 1.0f;
    bool hasSlots = false;
    std::vector<int32_t> slots;   // Key -> atlas cell, or -1.
    std::vector<int32_t> cells;   // Cells written by this op...
    std::vector<uint8_t> pixels;  // ...cell * cell bytes each, in the same order.
};

class StageRenderer {
public:
    StageRenderer(int atlasWidth, int atlasHeight, int cell)
            : atlasWidth_(atlasWidth), atlasHeight_(atlasHeight), cell_(cell),
              cols_(std::max(1, atlasWidth / std::max(1, cell))),
              cells_(static_cast<size_t>(cols_) * std::max(1, atlasHeight / std::max(1, cell))) {
        thread_ = std::thread([this] { run(); });
        waitReady();
    }

    ~StageRenderer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake();
        thread_.join();
    }

    /// Returns once the thread no longer draws to the old window, so the caller may let it go.
    void setWindow(ANativeWindow* window) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pendingWindowSet_ && pendingWindow_ != nullptr) ANativeWindow_release(pendingWindow_);
        pendingWindow_ = window;
        pendingWindowSet_ = true;
        const uint64_t ticket = ++windowTicket_;
        wake();
        applied_.wait_for(lock, std::chrono::seconds(2), [&] { return windowApplied_ >= ticket || quit_; });
    }

    int cellCount() const { return static_cast<int>(cells_.size()); }
    int cellSize() const { return cell_; }
    int atlasWidth() const { return atlasWidth_; }
    int atlasHeight() const { return atlasHeight_; }

    void post(Op&& op) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ops_.push_back(std::move(op));
        }
        wake();
    }

    void clearScene() {
        Op op;
        op.kind = Op::kClear;
        post(std::move(op));
    }

    void setActive(bool active) {
        if (active_.exchange(active) != active && active) requestFrame();
        else wake();
    }

    void setClock(int64_t posMs, bool playing, float speed, bool force) {
        const int64_t now = nowNs();
        bool snap;
        {
            std::lock_guard<std::mutex> lock(clockMutex_);
            const double predicted = clockMsLocked(now);
            const double drift = static_cast<double>(posMs) - predicted;
            snap = force || !playing || !clockPlaying_ || !clockStarted_
                    || std::fabs(speed - clockSpeed_) > 0.001f || std::fabs(drift) > kClockSnapMs;
            clockAnchorMs_ = snap ? static_cast<double>(posMs)
                                  : predicted + std::clamp(drift * kClockCorrection, -kClockMaxStepMs, kClockMaxStepMs);
            clockAnchorNs_ = now;
            clockSpeed_ = speed;
            clockPlaying_ = playing;
            clockStarted_ = true;
        }
        playing_.store(playing);
        if (snap) requestFrame();
    }

    void setPulse(float pulse) { pulse_.store(pulse, std::memory_order_relaxed); }

    void setBands(float low, float mid, float high) {
        bandLow_.store(low, std::memory_order_relaxed);
        bandMid_.store(mid, std::memory_order_relaxed);
        bandHigh_.store(high, std::memory_order_relaxed);
    }

    /// Light theme: black lines on a white ground.
    void setInvert(bool invert) {
        if (invert_.exchange(invert) != invert) requestFrame();
    }

    void setMask(float progress, float start, float end, float left, float right) {
        maskProgress_.store(progress);
        maskStart_.store(start);
        maskEnd_.store(end);
        maskLeft_.store(left);
        maskRight_.store(right);
        requestFrame();
    }

    void requestFrame() {
        dirty_.store(true);
        wake();
    }

private:
    void wake() {
        ALooper* looper = looper_.load();
        if (looper != nullptr) ALooper_wake(looper);
    }

    void waitReady() {
        std::unique_lock<std::mutex> lock(mutex_);
        applied_.wait(lock, [&] { return ready_; });
    }

    double clockMsLocked(int64_t ns) const {
        if (!clockPlaying_) return clockAnchorMs_;
        return clockAnchorMs_ + static_cast<double>(ns - clockAnchorNs_) / 1e6 * clockSpeed_;
    }

    double clockMsAt(int64_t ns) {
        std::lock_guard<std::mutex> lock(clockMutex_);
        return clockMsLocked(ns);
    }

    void run() {
        ALooper* looper = ALooper_prepare(0);
        ALooper_acquire(looper);
        looper_.store(looper);
        choreographer_ = AChoreographer_getInstance();
        frame_ = rust().ok ? rust().frame_new() : nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
        }
        applied_.notify_all();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (quit_) break;
            }
            applyPending();
            scheduleFrame();
            ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        }
        teardownSurface();
        teardownGl();
        if (stage_ != nullptr) rust().stage_free(stage_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Op& op : ops_) {
                if (op.stage != nullptr) rust().stage_free(op.stage);
            }
            ops_.clear();
            if (pendingWindowSet_ && pendingWindow_ != nullptr) ANativeWindow_release(pendingWindow_);
            pendingWindowSet_ = false;
            windowApplied_ = windowTicket_;
        }
        applied_.notify_all();
        if (frame_ != nullptr) rust().frame_free(frame_);
        looper_.store(nullptr);
        ALooper_release(looper);
    }

    void applyPending() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingWindowSet_) {
            teardownSurface();
            window_ = pendingWindow_;
            pendingWindow_ = nullptr;
            pendingWindowSet_ = false;
            if (window_ != nullptr) setupSurface();
            windowApplied_ = windowTicket_;
            applied_.notify_all();
        }
        if (ops_.empty()) return;
        for (Op& op : ops_) applyOp(op);
        ops_.clear();
        dirty_.store(true);
    }

    void applyOp(Op& op) {
        // Cell pictures first: the key table that may come with them can only point at drawn cells.
        const size_t cellBytes = static_cast<size_t>(cell_) * cell_;
        for (size_t i = 0; i < op.cells.size() && (i + 1) * cellBytes <= op.pixels.size(); i++) {
            const int cell = op.cells[i];
            if (cell < 0 || cell >= cellCount()) continue;
            cells_[cell].assign(op.pixels.begin() + static_cast<std::ptrdiff_t>(i * cellBytes),
                                op.pixels.begin() + static_cast<std::ptrdiff_t>((i + 1) * cellBytes));
            uploadQueue_.push_back(cell);
        }
        switch (op.kind) {
            case Op::kScene: {
                const bool fresh = stage_ == nullptr || op.fadeIn;
                if (stage_ != nullptr) rust().stage_free(stage_);
                stage_ = op.stage;
                op.stage = nullptr;
                stageSerial_ = op.serial;
                stageWidth_ = op.stageWidth;
                stageHeight_ = op.stageHeight;
                slots_ = std::move(op.slots);
                if (fresh) fadeStartNs_ = nowNs();
                break;
            }
            case Op::kClear:
                if (stage_ != nullptr) rust().stage_free(stage_);
                stage_ = nullptr;
                stageSerial_ = 0;
                slots_.clear();
                break;
            case Op::kGlyphs:
                if (op.hasSlots && op.serial == stageSerial_ && stage_ != nullptr) slots_ = std::move(op.slots);
                break;
        }
    }

    bool fading() const { return stage_ != nullptr && nowNs() - fadeStartNs_ < kFadeInNs; }

    void scheduleFrame() {
        if (framePosted_ || surface_ == EGL_NO_SURFACE) return;
        // Without a world (between songs) only changes draw: the blank ground, once.
        const bool animate = stage_ != nullptr && active_.load() && (playing_.load() || fading());
        if (!animate && !(active_.load() && dirty_.load())) return;
        framePosted_ = true;
        AChoreographer_postFrameCallback(choreographer_, &StageRenderer::onFrame, this);
    }

    static void onFrame(long frameTimeNanos, void* self) {
        auto* renderer = static_cast<StageRenderer*>(self);
        renderer->framePosted_ = false;
        renderer->frame(static_cast<int64_t>(frameTimeNanos));
        renderer->scheduleFrame();
    }

    void frame(int64_t frameNs) {
        if (surface_ == EGL_NO_SURFACE || !active_.load()) return;
        const bool forced = dirty_.exchange(false);
        const int64_t interval = highRate_ ? kFrame60Ns : kFrame30Ns;
        if (!forced && frameNs - lastDrawNs_ < interval) return;
        lastDrawNs_ = frameNs;
        draw(frameNs);
    }

    void setupSurface() {
        if (!ensureGl()) return;
        surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            LOGE("eglCreateWindowSurface: 0x%x", eglGetError());
            return;
        }
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
            LOGE("eglMakeCurrent: 0x%x", eglGetError());
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return;
        }
        // Paced by the Choreographer: never block in swap.
        eglSwapInterval(display_, 0);
        if (!ensurePrograms()) teardownSurface();
        dirty_.store(true);
    }

    void teardownSurface() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        }
        surface_ = EGL_NO_SURFACE;
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    bool ensureGl() {
        if (context_ != EGL_NO_CONTEXT) return true;
        if (display_ == EGL_NO_DISPLAY) {
            display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
                LOGE("eglInitialize: 0x%x", eglGetError());
                display_ = EGL_NO_DISPLAY;
                return false;
            }
        }
        const EGLint configAttrs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display_, configAttrs, &config_, 1, &count) || count < 1) {
            LOGE("eglChooseConfig: 0x%x", eglGetError());
            return false;
        }
        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttrs);
        if (context_ == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext: 0x%x", eglGetError());
            return false;
        }
        return true;
    }

    bool ensurePrograms() {
        if (shapeProgram_ != 0) return true;
        shapeProgram_ = link(kShapeVs, kShapeFs);
        glyphProgram_ = link(kGlyphVs, kGlyphFs);
        if (shapeProgram_ == 0 || glyphProgram_ == 0) return false;
        uSize_ = glGetUniformLocation(shapeProgram_, "uSize");
        uDither_ = glGetUniformLocation(shapeProgram_, "uDither");
        glUseProgram(glyphProgram_);
        glUniform1i(glGetUniformLocation(glyphProgram_, "uAtlas"), 0);

        glGenBuffers(1, &shapeVbo_);
        glGenBuffers(1, &shapeIbo_);
        glGenBuffers(1, &glyphVbo_);
        glGenBuffers(1, &quadIbo_);
        glGenVertexArrays(1, &shapeVao_);
        glGenVertexArrays(1, &glyphVao_);
        glGenTextures(1, &atlasTexture_);

        std::vector<uint16_t> quads(kMaxQuads * 6);
        for (int i = 0; i < kMaxQuads; i++) {
            const auto v = static_cast<uint16_t>(i * 4);
            const uint16_t idx[6] = {v, static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 2),
                                     v, static_cast<uint16_t>(v + 2), static_cast<uint16_t>(v + 3)};
            std::copy(idx, idx + 6, quads.begin() + i * 6);
        }

        glBindVertexArray(shapeVao_);
        glBindBuffer(GL_ARRAY_BUFFER, shapeVbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shapeIbo_);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(0));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));

        glBindVertexArray(glyphVao_);
        glBindBuffer(GL_ARRAY_BUFFER, glyphVbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIbo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(quads.size() * sizeof(uint16_t)),
                     quads.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        // Rust's GlyphV: clip xyz, uv + sweep coordinate, fill/stroke/glow/sweep, blur (11 floats).
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 44, reinterpret_cast<void*>(0));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 44, reinterpret_cast<void*>(12));
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 44, reinterpret_cast<void*>(24));
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 44, reinterpret_cast<void*>(40));
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlasTexture_);
        int levels = 1;
        while ((std::max(atlasWidth_, atlasHeight_) >> levels) > 0) levels++;
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_R8, atlasWidth_, atlasHeight_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        {
            // An empty texture reads as "far outside every glyph" (0), never as garbage.
            std::vector<uint8_t> zeros(static_cast<size_t>(atlasWidth_) * 64, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            for (int y = 0; y < atlasHeight_; y += 64) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, atlasWidth_, std::min(64, atlasHeight_ - y), GL_RED,
                                GL_UNSIGNED_BYTE, zeros.data());
            }
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DITHER);
        glEnable(GL_BLEND);
        // A new context starts empty: every cell kept on this side goes up again.
        uploadQueue_.clear();
        for (int i = 0; i < cellCount(); i++) {
            if (!cells_[i].empty()) uploadQueue_.push_back(i);
        }
        return true;
    }

    void teardownGl() {
        if (display_ == EGL_NO_DISPLAY) return;
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
        shapeProgram_ = glyphProgram_ = 0;
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }

    /// Only the cells that changed, each a small sub-image; the mip chain follows once per batch.
    void uploadCells() {
        if (uploadQueue_.empty()) return;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlasTexture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const int cell : uploadQueue_) {
            const std::vector<uint8_t>& bytes = cells_[cell];
            if (bytes.empty()) continue;
            glTexSubImage2D(GL_TEXTURE_2D, 0, (cell % cols_) * cell_, (cell / cols_) * cell_, cell_, cell_, GL_RED,
                            GL_UNSIGNED_BYTE, bytes.data());
        }
        uploadQueue_.clear();
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    void draw(int64_t frameNs) {
        uploadCells();
        EGLint width = 0, height = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &width);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &height);
        if (width <= 0 || height <= 0) return;

        RFrameView view{};
        if (rust().frame_set_invert != nullptr) rust().frame_set_invert(frame_, invert_.load() ? 1 : 0);
        if (stage_ != nullptr) {
            const double ms = clockMsAt(frameNs + (playing_.load() ? kShowLeadNs : 0));
            const float fade = std::min(1.0f, static_cast<float>(nowNs() - fadeStartNs_) / static_cast<float>(kFadeInNs));
            if (rust().frame_set_bands != nullptr) {
                rust().frame_set_bands(frame_, bandLow_.load(std::memory_order_relaxed),
                                       bandMid_.load(std::memory_order_relaxed), bandHigh_.load(std::memory_order_relaxed));
            }
            rust().frame_render(frame_, stage_, slots_.data(), static_cast<int32_t>(slots_.size()),
                                static_cast<float>(ms / 1000.0), pulse_.load(std::memory_order_relaxed),
                                fade * fade * (3.0f - 2.0f * fade), maskProgress_.load(), maskStart_.load(),
                                maskEnd_.load(), maskLeft_.load(), maskRight_.load(), &view);
        } else {
            stageWidth_ = static_cast<float>(width);
            stageHeight_ = static_cast<float>(height);
            rust().frame_blank(frame_, stageWidth_, stageHeight_, maskProgress_.load(), maskStart_.load(),
                               maskEnd_.load(), maskLeft_.load(), maskRight_.load(), &view);
        }
        highRate_ = view.high_rate != 0;

        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBuffer(GL_ARRAY_BUFFER, shapeVbo_);
        glBufferData(GL_ARRAY_BUFFER, view.shape_vert_count * 16, view.shape_verts, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindVertexArray(shapeVao_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, view.shape_index_count * 2, view.shape_indices, GL_STREAM_DRAW);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, glyphVbo_);
        glBufferData(GL_ARRAY_BUFFER, view.glyph_vert_count * 44, view.glyph_verts, GL_STREAM_DRAW);

        glUseProgram(shapeProgram_);
        // Rust's pixels: the size the scene was built for, which matches the surface but for a
        // frame or two around a resize, when the whole picture simply scales.
        glUniform2f(uSize_, stageWidth_, stageHeight_);

        uint32_t bound = 0xFFFFFFFFu;
        for (int i = 0; i < view.cmd_count; i++) {
            const RCmd& cmd = view.cmds[i];
            if (cmd.kind != bound) {
                bound = cmd.kind;
                if (cmd.kind == 1 || cmd.kind == 4) {
                    glUseProgram(glyphProgram_);
                    glBindVertexArray(glyphVao_);
                    if (cmd.kind == 4) {
                        // Knock-out: darkens what is under the glyph (dst x (1 - coverage)), alpha kept.
                        glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
                    } else {
                        // Inverting blend: white on black, black on the solid white shapes.
                        glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
                    }
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, atlasTexture_);
                } else {
                    glUseProgram(shapeProgram_);
                    glBindVertexArray(shapeVao_);
                    if (cmd.kind == 2) {
                        // Multiplies the picture by the mask's alpha: its soft edges need no HWUI layer.
                        glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
                        glUniform1f(uDither_, 1.0f);
                    } else if (cmd.kind == 3) {
                        // Light theme: one white quad turns the colours over (1 - dst), alpha kept.
                        glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
                        glUniform1f(uDither_, 0.0f);
                    } else {
                        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                        glUniform1f(uDither_, 0.0f);
                    }
                }
            }
            if (cmd.kind == 1 || cmd.kind == 4) {
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.count * 6), GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.first) * 12));
            } else {
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.count), GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.first) * 2));
            }
        }
        glBindVertexArray(0);
        if (!eglSwapBuffers(display_, surface_)) {
            const EGLint error = eglGetError();
            if (error == EGL_CONTEXT_LOST) {
                // Rebuild everything on the same window; the cells' bytes are kept for this.
                ANativeWindow* window = window_;
                ANativeWindow_acquire(window);
                teardownSurface();
                if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
                context_ = EGL_NO_CONTEXT;
                shapeProgram_ = glyphProgram_ = 0;
                window_ = window;
                setupSurface();
            } else if (error != EGL_BAD_SURFACE && error != EGL_BAD_NATIVE_WINDOW) {
                LOGE("eglSwapBuffers: 0x%x", error);
            }
        }
    }

    std::thread thread_;
    std::atomic<ALooper*> looper_{nullptr};
    AChoreographer* choreographer_ = nullptr;

    std::mutex mutex_;
    std::condition_variable applied_;
    bool ready_ = false;
    bool quit_ = false;
    ANativeWindow* pendingWindow_ = nullptr;
    bool pendingWindowSet_ = false;
    uint64_t windowTicket_ = 0;
    uint64_t windowApplied_ = 0;
    std::vector<Op> ops_;

    std::atomic<bool> active_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<bool> playing_{false};
    std::mutex clockMutex_;
    double clockAnchorMs_ = 0.0;
    int64_t clockAnchorNs_ = 0;
    float clockSpeed_ = 1.0f;
    bool clockPlaying_ = false;
    bool clockStarted_ = false;
    std::atomic<float> pulse_{0.0f};
    std::atomic<float> bandLow_{0.0f};
    std::atomic<float> bandMid_{0.0f};
    std::atomic<float> bandHigh_{0.0f};
    std::atomic<bool> invert_{false};
    std::atomic<float> maskProgress_{1.0f};
    std::atomic<float> maskStart_{0.36f};
    std::atomic<float> maskEnd_{0.995f};
    std::atomic<float> maskLeft_{0.0f};
    std::atomic<float> maskRight_{0.0f};

    // Render thread only.
    ANativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint shapeProgram_ = 0, glyphProgram_ = 0;
    GLint uSize_ = -1, uDither_ = -1;
    GLuint shapeVbo_ = 0, shapeIbo_ = 0, glyphVbo_ = 0, quadIbo_ = 0;
    GLuint shapeVao_ = 0, glyphVao_ = 0, atlasTexture_ = 0;
    RStage* stage_ = nullptr;
    uint64_t stageSerial_ = 0;
    std::vector<int32_t> slots_;
    RFrame* frame_ = nullptr;
    const int atlasWidth_, atlasHeight_, cell_, cols_;
    std::vector<std::vector<uint8_t>> cells_;
    std::vector<int> uploadQueue_;
    float stageWidth_ = 1.0f, stageHeight_ = 1.0f;
    bool framePosted_ = false;
    bool highRate_ = true;
    int64_t lastDrawNs_ = 0;
    int64_t fadeStartNs_ = 0;
};

inline StageRenderer* renderer(jlong handle) { return reinterpret_cast<StageRenderer*>(handle); }

}  // namespace
