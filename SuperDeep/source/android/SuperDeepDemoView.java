package com.lab.hv;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;

import java.util.ArrayList;
import java.util.List;

/**
 * The SuperDeep introduction's preview: the real stage ({@link LyricStageView}, the same native
 * renderer the player uses) playing a short demo song on a 20 s loop, alternating between the city
 * and the hall; each loop goes from the 3D world through the cut into a poster and back. It runs
 * only while the prompt is up and is released with it. Dark or light like the app.
 */
public final class SuperDeepDemoView extends FrameLayout {

    private static final long LOOP_MS = 20_000L;
    /** The demo's lines: start and end, in ms. Lines 3 and 4 go on the poster (see scene::MODE_DEMO_*). */
    private static final long[][] TIMES = {
            {500, 2300}, {2900, 4700}, {5300, 7100}, {8400, 10200}, {10800, 12600}, {13900, 15700}, {16300, 18100},
    };

    private final LyricStageView stage;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float radius;
    private boolean city = true;
    private boolean running;

    private final Runnable loop = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            // Every other loop in the hall; each one from the top.
            city = !city;
            stage.setSceneKind(city ? LyricStageView.SCENE_DEMO_CITY : LyricStageView.SCENE_DEMO_HALL);
            stage.setClock(0L, true, 1f, true);
            handler.postDelayed(this, LOOP_MS);
        }
    };

    public SuperDeepDemoView(@NonNull Context context, boolean dark, @NonNull String[] lines) {
        super(context);
        radius = 20f * getResources().getDisplayMetrics().density;
        stage = new LyricStageView(context);
        stage.setLight(!dark);
        stage.setSceneKind(LyricStageView.SCENE_DEMO_CITY);
        stage.setEdges(1f, 1f, 0f, 0f);
        addView(stage, new LayoutParams(0, 0));
        stage.setLyrics(demoLines(lines), null, "superdeep-demo", false);
        float[] beats = new float[44];
        for (int i = 0; i < beats.length; i++) beats[i] = i * 0.5f;
        float[] bars = new float[11];
        for (int i = 0; i < bars.length; i++) bars[i] = i * 2f;
        stage.setBeats(beats, bars, "superdeep-demo");
    }

    /** Each line sung glyph by glyph (word by word for spaced text) across its time. */
    @NonNull
    private static List<LyricStageLine> demoLines(@NonNull String[] texts) {
        List<LyricStageLine> out = new ArrayList<>();
        for (int i = 0; i < Math.min(texts.length, TIMES.length); i++) {
            String text = texts[i];
            long t0 = TIMES[i][0];
            long t1 = TIMES[i][1];
            int count = text.codePointCount(0, text.length());
            List<int[]> tokens = new ArrayList<>();
            boolean spaced = text.indexOf(' ') >= 0;
            int start = -1;
            for (int cp = 0; cp < count; cp++) {
                int c = text.codePointAt(text.offsetByCodePoints(0, cp));
                if (spaced) {
                    if (c == ' ') {
                        if (start >= 0) tokens.add(new int[]{start, cp});
                        start = -1;
                    } else if (start < 0) {
                        start = cp;
                    }
                } else {
                    tokens.add(new int[]{cp, cp + 1});
                }
            }
            if (spaced && start >= 0) tokens.add(new int[]{start, count});
            int n = tokens.size();
            long[] times = new long[n];
            int[] starts = new int[n];
            int[] ends = new int[n];
            for (int k = 0; k < n; k++) {
                times[k] = t0 + (t1 - t0) * k / Math.max(1, n);
                starts[k] = tokens.get(k)[0];
                ends[k] = tokens.get(k)[1];
            }
            out.add(new LyricStageLine(t0, text, times, starts, ends, t1));
        }
        return out;
    }

    /** As wide as the prompt, its height following the width (never a strip on a tablet). */
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        float density = getResources().getDisplayMetrics().density;
        int height = Math.round(Math.max(170f * density, Math.min(250f * density, width * 0.5f)));
        setMeasuredDimension(width, height);
    }

    /** The stage is a card no wider than 2.3 : 1, centred, with rounded corners. */
    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int w = r - l;
        int h = b - t;
        int cardW = Math.min(w, Math.round(h * 2.3f));
        int left = (w - cardW) / 2;
        stage.setFrame(left, 0, left + cardW, h, 0f);
        stage.setMorph(left, 0, left + cardW, h, radius);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        sync();
    }

    @Override
    public void onVisibilityAggregated(boolean isVisible) {
        super.onVisibilityAggregated(isVisible);
        sync();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        running = false;
        handler.removeCallbacks(loop);
        // The prompt is gone for good: thread, surface and atlas go with it.
        stage.release();
    }

    /** Plays (from the top) while on screen, holds while hidden. */
    private void sync() {
        boolean want = isAttachedToWindow() && isShown();
        if (want == running) return;
        running = want;
        stage.setActive(want);
        handler.removeCallbacks(loop);
        if (want) {
            stage.setClock(0L, true, 1f, true);
            handler.postDelayed(loop, LOOP_MS);
        } else {
            stage.setClock(0L, false, 1f, true);
        }
    }
}
