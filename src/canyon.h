// Re-implementation of the PS3 "canyon" music visualizer (qgl_canyon_app.sprx).
//
// The CPU side (terrain simulation, camera, parameters, render passes) is
// rewritten from the firmware module's PowerPC code; the GPU side runs the
// firmware's own shaders from canyon.qrc, translated to GLSL at load time.
#pragma once

#include "cg.h"
#include "albumart.h"
#include "firmware.h"

#include <map>
#include <random>
#include <string>
#include <vector>

class Canyon {
public:
    // User adjustments layered on top of the preset values.
    struct Tweaks {
        float waveHeight = 1.0f; // TERRAIN HEIGHT and LINE HEIGHT
        float lineWidth = 1.0f;  // LINE WIDTH
        float speed = 1.0f;      // POS SPEED
        float blur = 1.0f;       // depth of field (LOD and focus falloff)
        int blurDiv = 4;         // blur stack resolution: 1 full, 2 half, 4 quarter, 8 eighth
        bool blurHq = false;     // run the blur stack at full resolution, same blur size as blurDiv
        float glow = 0.8f;       // bloom strength, 0 = off
        float fov = 1.0f;        // scales the field of view
        int fallingTiles = 1; // tile-scatter effect (TIME AHEAD < 128): 0 off, 1 auto, 2 on
        bool keepAboveTerrain = true; // lift the camera out of hills
        float syncMs = 0.0f;          // added to the PS3 look-ahead (e.g. for output latency)
        // Music response: 0 = the PS3's own spectrum feed and scaling, 1 = the same with the
        // bass columns smoothed over ~50 ms, 2 = enhanced (this app's log-band feed).
        int spectrumMode = 0;
        bool cameraMotion = true;  // master switch for the drift below
        float drift = 1.0f;        // sideways camera drift distance
        float driftSpeed = 1.0f;   // drift speed multiplier
        float cameraSmooth = 0.16f; // seconds for the camera to ease towards its goal (0 = instant)
        bool operator==(const Tweaks &) const = default;
    };
    Tweaks tweaks;

    static constexpr int kCols = 128;
    static constexpr int kRows = 128;

    bool init(const fs::path &devFlash, std::string &error);
    void shutdown();

    const std::vector<std::string> &presets() const { return presetNames_; }
    int preset() const { return preset_; }
    // Switch to a preset; parameters morph over `seconds` (0 = instantly).
    void setPreset(int index, float seconds = 2.5f);

    // The user's own preset: always the last entry, "Custom". Its values are
    // the base parameters overridden by `values` (applied live while active).
    static constexpr const char *kCustomName = "Custom";
    int customIndex() const { return int(presetNames_.size()) - 1; }
    void setCustom(const std::map<std::string, float> &values);
    bool customInRotation = false;
    // Index `dir` steps from `from`, skipping Custom unless it is in rotation.
    int step(int from, int dir) const;
    // Last preset that wasn't Custom (for "copy from previous preset").
    int lastBuiltin() const { return lastBuiltin_; }
    // Debugging: per-frame numbers for history, and a full text dump.
    struct Stats {
        float morph, scroll, rowBase, peak, peakEnv, meanNorm, maxNorm, lift, wantLift, catchUp;
        int ring, feedbackFrames;
    };
    Stats stats() const;
    // This update's input columns, and the row values after the preset boost/scaling.
    const float *inputColumns() const { return rawColumns_; }
    const float *rowValues() const { return spectrum_; }
    // Album-art colours: the preset's colours are shifted to these hues (gliding over ~3 s);
    // nullptr fades back to the preset's own colours.
    void setPalette(const AlbumPalette *palette);
    // The scene's current fog and line colours (with the preset morph and album tint; no fades).
    void sceneColours(float fog[3], float line[3]) const;
    // Output goes to an HDR display: the glow may exceed SDR white.
    bool hdrOutput = false;
    // How far ahead of playback the spectrum is taken for the current preset (seconds).
    float prefetchSeconds() const;
    void dumpState(std::string &out) const;
    // Parameter names, and the values of a preset.
    const std::vector<std::string> &paramNames() const { return base_.names; }
    std::map<std::string, float> presetValues(int index) const;

    // `columns` = 128 spectrum magnitudes laid out like the PS3 feeds them.
    void update(float dt, const float *columns);
    // Renders one visualizer frame into an internal buffer...
    void render(int width, int height);
    // ...and copies the latest one to `targetFbo` (any number of times).
    void present(int width, int height, GLuint targetFbo, float dither = 0); // dither: Misc > Dithering amount

private:
    struct Params {
        std::vector<std::string> names;
        std::vector<float> values;
        float get(const char *name) const;
    };
    struct Row {
        float pos[4];
        float nrm[4];
    };

    Params loadPreset(const std::string &name) const;
    float p(const char *name) const { return cur_.get(name); }
    void stepParams(float dt);
    void buildMesh();
    void generateRow();
    void rebase(float delta);
    void writeHalf(size_t index, float v) { verts_[index] = toHalf(v); }
    void updateFresLut();
    void resize(int w, int h);
    int layerDiv() const;
    int builtDiv_ = 0;
    void drawTerrain();
    void renderGlow(int w, int h);
    float rnd() { return dist_(rng_); }
    static uint16_t toHalf(float f);

    QrcArchive qrc_;
    std::vector<std::string> presetNames_;
    Params base_;
    Params cur_, from_, to_;
    float morph_ = 1.0f, morphTime_ = 0.0f;
    int preset_ = -1;
    int lastBuiltin_ = 0;
    std::map<std::string, float> custom_;

    // Terrain simulation state (field names follow the original object).
    float clock_ = 0.0f;      // drift phase (seconds scaled by drift speed)
    float camEye_[3] = {}, camTarget_[3] = {}; // smoothed, relative to the scroll position
    bool camValid_ = false;
    float lift_ = 0.0f;     // current keep-above-terrain lift
    float wantLift_ = 0.0f, lastPeak_ = 0.0f;
    float rawColumns_[kCols] = {};
    float catchUp_ = 0.0f;  // fast lift tracking after a preset change (seconds left, at most)
    float morphWas_ = 1.0f;
    void updateCamera(float dt);
    float scroll_ = -10.0f;   // distance travelled, in rows
    float rowBase_ = 0.0f;    // z of the most recent row minus 127
    int ring_ = 0;            // next ring slot to overwrite
    float spectrum_[kCols] = {};
    float peakEnv_ = 0.0f, rawEnv_ = 0.0f, peakSmooth_ = 0.0f;
    AlbumPalette palette_{};       // shown now (glides towards paletteTarget_)
    AlbumPalette paletteTarget_{};
    bool havePalette_ = false;
    float paletteMix_ = 0.0f; // 0 = preset colours, 1 = album colours
    float colEnv_[kCols] = {};
    std::vector<Row> rowA_, rowB_, rowC_; // previous, current, newest profiles
    std::vector<float> velocity_, rowZ_;
    std::vector<float> heights_, targets_; // per ring slot and column: profile height y and target (P.y, P.w)
    std::vector<uint16_t> verts_;
    bool vertsDirty_ = true;           // whole buffer
    std::vector<int> dirtyStrips_;     // or just these strips
    std::mt19937 rng_{0x5053};
    std::uniform_real_distribution<float> dist_{0.0f, 1.0f};

    // GL resources
    CgShader terrainVp_, terrainFp_, amountVp_, amountFp_, blurFp_, layerFp_, coneFp_;
    CgProgram terrain_, amount_, blur_, layer_, cone_;
    GLuint brightProg_ = 0, glowProg_ = 0, lumProg_ = 0, lumTex_ = 0, lumFbo_ = 0;
    static constexpr int kGlowLevels = 6;
    GLuint glowTex_[kGlowLevels] = {}, glowFbo_[kGlowLevels] = {};
    GLuint finalTex_ = 0, finalFbo_ = 0;
    GLuint upProg_ = 0, layersHi_ = 0, hiTex_ = 0, hiFbo_ = 0; // smooth enlargement of the blur stack
    GLuint vao_ = 0, vbo_ = 0, quadVao_ = 0, quadVbo_ = 0;
    GLuint normalMap_ = 0, fresLut_ = 0, noise1_ = 0, noise2_ = 0;
    GLuint colorTex_ = 0, screenTex_ = 0, depthRb_ = 0, layers_ = 0;
    GLuint pingTex_[2] = {}, accumTex_[2] = {};
    GLuint colorFbo_ = 0, screenFbo_ = 0, pingFbo_[2] = {}, accumFbo_[2] = {};
    int width_ = 0, height_ = 0, accum_ = 0;
    // Feedback ramps from FEEDBACK to 0 over 100 frames when the visualizer
    // starts (state 1 of the original's fade machine); also used on preset changes.
    int feedbackFrames_ = 100;
    bool accumValid_ = false;
};
