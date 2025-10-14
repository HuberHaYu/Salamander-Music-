# Salam Player | 蝾螈播放器
针对本地、远程数据而生的超棒的播放器App | A great music player app designed for both local and remote data.
### 基于底层AudioTrack开发的音频API，拥有独立的DSP算法，能够做到低延迟的同时尽可能地输出高质量音频
### <br><br>[最新版本 | Latest Release](https://github.com/HuberHaYu/Salamander-Music-/releases/tag/v1.0.1)
## 关于音频处理：
Salamander Player（Salam Player）将以AudioTrack为底层基础进行研发的音频API系统（Salam API）：
### 音频入口：
<br><br>使用了全链路同步低延迟系统 及DSP芯片的读取、处理功能，拥有独立的USB独占处理方式（支持外挂DSP转接及自带DSP芯片处理的有线耳机）
#### DSP芯片读取示例：
```java
    public interface UsbProbeCallback {
        void onReady(String chipVendor, String productName, String headphoneMaker, int vendorId, int productId, String serial);
    }
    public static final class UsbDspMeta {
        public static volatile String chipVendor = "";
        public static volatile String productName = "";
        public static volatile String headphoneMaker = "";
        public static volatile int vendorId = 0;
        public static volatile int productId = 0;
        public static volatile String serial = "";
    }
    public static String getUsbChipVendor()     { return UsbDspMeta.chipVendor; }
    public static String getUsbProductName()    { return UsbDspMeta.productName; }
    public static String getUsbHeadphoneMaker() { return UsbDspMeta.headphoneMaker; }
    public static int    getUsbVendorId()       { return UsbDspMeta.vendorId; }
    public static int    getUsbProductId()      { return UsbDspMeta.productId; }
    public static String getUsbSerial()         { return UsbDspMeta.serial; }

    private static String nz(@Nullable String s, String def) { return (s == null || s.isEmpty()) ? def : s; }

    @Nullable
    private static String readUsbStringDesc(UsbAudioDspSession s, int index) {
        if (index <= 0) return null;
        byte[] buf = new byte[255];
        int wValue = (0x03 << 8) | (index & 0xFF);
        int r = s.controlIn(0x80, 0x06, wValue, 0x0409, buf, 200);
        if (r >= 2 && (buf[1] & 0xFF) == 0x03) {
            int len = Math.min(r, buf[0] & 0xFF);
            if (len > 2) {
                try { return new String(buf, 2, len - 2, "UTF-16LE"); } catch (Exception ignore) {}
            }
        }
        return null;
    }
```
### 音频处理：
<br>Salam API拥有独立的DSP处理入口，在设备性能支持的情况下，可接入无数个DSP处理器。其中包括低频增强、虚拟低频、HRTF环绕等核心算法
#### 算法示例（以IIR为例）：
```java
public byte[] process(byte[] pcmData) {
        try {
            short[] s = new short[pcmData.length / 2];
            ByteBuffer.wrap(pcmData).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(s);

            final int frames = s.length / 2;
            final int BLOCK = 192; // 防抖

            for (int base = 0; base < frames; base += BLOCK) {
                int end = Math.min(base + BLOCK, frames);
                int count = end - base;

                // —— RMS ——
                float sum = 0f;
                for (int n = 0; n < count; n++) {
                    int i = (base + n) * 2;
                    float L = s[i] / 32768f;
                    float R = s[i + 1] / 32768f;
                    float a = 0.5f * (Math.abs(L) + Math.abs(R));
                    sum += a * a;
                }
                float rms = (float)Math.sqrt(sum / Math.max(1, count));
                rmsEnv += rmsCoef * (rms - rmsEnv);

                // —— 动态低搁架 ——
                float t = clamp((loudnessRef - rmsEnv) / loudnessRange, 0f, 1f);
                t = t * t * (3f - 2f * t); // Smooth Step
                float dynDb = lsBaseDb + (enableLoudness ? (loudnessStrength * (3.5f - lsBaseDb) * t) : 0f);
                dynDb = clamp(dynDb, -20f, 20f);
                if (Math.abs(dynDb - lastLsDb) > 0.1f) {
                    updateLowShelf(dynDb);
                    lastLsDb = dynDb;
                }

                // —— 处理块 —— //
                for (int n = 0; n < count; n++) {
                    int i = (base + n) * 2;

                    float L = s[i] / 32768f;
                    float R = s[i + 1] / 32768f;

                    // IIR EQ
                    L = hpL.process(L);    R = hpR.process(R);
                    L = lsL.process(L);    R = lsR.process(R);
                    L = lmidL.process(L);  R = lmidR.process(R);
                    L = midDipL.process(L); R = midDipR.process(R);
                    L = upMidDipL.process(L); R = upMidDipR.process(R);
                    L = hsL.process(L);    R = hsR.process(R);

                    // ======= peaking cut =======
                    for (int k = 0; k < highlights.size(); k++) {
                        HighlightBand hb = highlights.get(k);
                        if (hb.isCutMode) {
                            L = hb.peakL.process(L);
                            R = hb.peakR.process(R);
                        } else {
                            L = hb.lsNegL.process(L);
                            R = hb.lsNegR.process(R);

                            L = hb.peakPosL.process(L);
                            R = hb.peakPosR.process(R);

                            L = hb.hsNegL.process(L);
                            R = hb.hsNegR.process(R);
                        }
                    }

                    // 末端限幅
                    float peak = Math.max(Math.abs(L), Math.abs(R));
                    limEnv += ((peak > limEnv) ? limAtkCoef : limRelCoef) * (peak - limEnv);
                    float targetGain = (limEnv > LIM_CEIL) ? (LIM_CEIL / (limEnv + 1e-9f)) : 1.0f;
                    float gCoef = (targetGain < limGain) ? limAtkCoef : limRelCoef;
                    limGain += gCoef * (targetGain - limGain);

                    L *= limGain; R *= limGain;

                    L = softClip(L); R = softClip(R);

                    s[i]     = toShort(L);
                    s[i + 1] = toShort(R);
                }
            }

            ByteBuffer.wrap(pcmData).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(s);
            return pcmData;
        } catch (Exception e) {
            Log.e(TAG, "SpeakerFRShaper error: " + e.getMessage());
            return pcmData;
        }
    }
```
<br><br>
## 关于UI设计：
Salam Player全界面使用了现代化排布，使用了全动态UI效果
<table>
  <tr>
    <td><img src="https://huberhayu.github.io/Salamander-Music-/image/glass.jpg" width="360"/></td>
    <td><img src="https://huberhayu.github.io/Salamander-Music-/image/album_list.jpg" width="360"/></td>
  </tr>
  <tr>
    <td><img src="https://huberhayu.github.io/Salamander-Music-/image/A.jpg" width="360"/></td>
    <td><img src="https://huberhayu.github.io/Salamander-Music-/image/B.jpg" width="360"/></td>
  </tr>
  <tr>
    <td colspan="2" align="center">
      <img src="https://huberhayu.github.io/Salamander-Music-/image/music_layout.jpg" width="540"/>
    </td>
  </tr>
</table>
<br><br>
<video
  src="https://github.com/user-attachments/assets/VD.mp4"
  width="720"
  autoplay
  loop
  muted
  playsinline
  preload="metadata"
  controlslist="nodownload noplaybackrate noremoteplayback"
  disablepictureinpicture>
</video>

## 更多信息请关注GitHub及 -> [BiliBili](https://space.bilibili.com/194639276?spm_id_from=333.1007.0.0) <-
