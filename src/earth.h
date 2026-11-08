// The PS3's second music visualizer: the Earth seen from orbit (qgl_gaia_app + earth.qrc).
//
// Stage 1: our own renderer on the firmware's assets: the cube-mapped ground / cloud / specular
// textures, the presets' camera flights (camera.path) and their sun, exposure and glare values.
// The atmosphere is an approximation (the PS3 computes it on an SPU); see devnotes/EARTH.md.
#pragma once

#include "cg.h"
#include "firmware.h"
#include "gl.h"

#include <algorithm>
#include <string>
#include <vector>

class Earth {
public:
    struct Tweaks {
        float flightSpeed = 1.0f; // camera path speed
        bool clouds = true;
        float cloudSpeed = 1.0f;  // cloud drift
        float stars = 1.0f;       // star brightness (0 = off)
        float glare = 1.0f;       // sun glare and bloom
        float exposure = 1.0f;    // on top of the preset's
        int music = 1;            // reaction to the music: 0 off, 1 gentle, 2 strong (the PS3 has none)
        bool nextAfterFlight = true; // move on to the next view when a flight ends
        bool firmwareShaders = false; // the PS3's own surface shader (hybrid: our atmosphere table and tone mapping)
        int antiAlias = 2; // multisampling of the scene: 0 off, 1 2x, 2 4x, 3 8x (limited to what the GPU offers)
        int realTime = 0; // day and night like now (UTC): 0 off, 1 turn the Earth under the view's sun, 2 move the sun
        bool operator==(const Tweaks &) const = default;
    };
    Tweaks tweaks;
    bool hdrOutput = false; // highlights may exceed SDR white (HDR presenter)

    bool init(const fs::path &devFlash, std::string &error);
    bool ready() const { return ready_; }
    void shutdown();

    const std::vector<std::string> &presets() const { return names_; } // e.g. "0", "0a", ...
    int preset() const { return preset_; }
    void setPreset(int index); // fades through dark into the new view

    // `level` is the audio level (0..1), `columns` the visualizer's 128 spectrum columns.
    void update(float dt, float level, const float *columns);
    void render(int width, int height);
    void present(int width, int height, GLuint targetFbo, float dither = 0); // dither: Misc > Dithering amount
    // Main colours of the view (for "UI colours from visuals"): sky/ocean and sun.
    void sceneColours(float main[3], float accent[3]) const;
    // For the debug spectrum: what the music reaction uses per column (smoothed; < 0 = column not used), the
    // latest beat (1 on a beat, decaying) and the glow's current bounce.
    static constexpr int kCols = 128;
    const float *appliedColumns() const { return applied_; }
    float beat() const { return kick_; }
    float glowBounce() const { return glow_; }
    float glowBoost() const { return std::max(glow_, -0.4f); } // (the rebound may dip a little below rest)
    // For debug dumps: view, flight time, cloud drift, shader path and real-time mode (repro of a frame).
    void dumpState(std::string &out) const;

private:
    struct Key {
        float time, eye[3], center[3], up[3], fovy;
    };
    struct Preset {
        std::string name;
        std::vector<Key> path;
        float sunY = 90, sunZ = 0, brightness = 3.4f, exposure = 0.8f, white = 3.4f, glareLevel = 1.0f;
        float animSpeed = 1.0f, cloudBrightness = 0.3f, cloudBlend = 0.35f;
        // for the firmware shader (EARTH.mnu, CLOUDS.mnu, ATM.mnu)
        float seaAlbedo = 0.091f, seaPow = 10, landAlbedo = 0.038f, landPow = 20;
        float parallax = 0.15f, parallaxBias = 0, shadowBias = 0.81f, shadowStrength = 1;
        float attnEarth = 0.038f, attnAtm = 2.176f;
    };
    bool initFirmware(const QrcArchive &qrc);
    void renderFirmware(const float viewProjRows[16], const float eye[3], const float sun[3], float bright, const float geo[9]);
    void resize(int w, int h);
    void cameraAt(float t, float eye[3], float center[3], float up[3], float &fovy) const;

    bool ready_ = false;
    std::vector<Preset> presets_;
    std::vector<std::string> names_;
    int preset_ = 0, pending_ = -1;
    float time_ = 0, fade_ = 0, fadeTarget_ = 1, cloudAngle_ = 0, pulse_ = 0, bass_ = 0;
    float applied_[kCols] = {}, lowFast_ = 0, lowSlow_ = 0, kick_ = 0, beatWait_ = 0, glow_ = 0, glowVel_ = 0;

    GLuint ground_ = 0, clouds_ = 0; // texture arrays (6 faces)
    GLuint sphereVao_ = 0, sphereVbo_ = 0, sphereIbo_ = 0, quadVao_ = 0, starVao_ = 0, starVbo_ = 0;
    int sphereIndices_ = 0, starCount_ = 0;
    GLuint earthProg_ = 0, atmProg_ = 0, starProg_ = 0, sunProg_ = 0, brightProg_ = 0, blurProg_ = 0, toneProg_ = 0;
    // Firmware shader path (lib/gaia/CubeEarthLayer): see devnotes/EARTH.md "Firmware shaders (hybrid)".
    CgShader fwVp_, fwFp_;
    CgProgram fwProg_;
    bool fwReady_ = false;
    GLuint specular_ = 0, patchVao_ = 0, patchVbo_ = 0, patchIbo_ = 0, ieTex_ = 0, specPowTex_ = 0, ivTex_ = 0, ivFbo_ = 0;
    GLuint ivProg_ = 0;
    int patchIndices_ = 0, specPowFor_ = -1;
    int width_ = 0, height_ = 0;
    GLuint msFbo_ = 0, msColorRb_ = 0, msDepthRb_ = 0, drawFbo_ = 0; // drawFbo_: the scene's target this frame
    int msSamples_ = 0, msW_ = 0, msH_ = 0;
    void ensureMultisample(int samples);
    GLuint sceneTex_ = 0, sceneFbo_ = 0, depthRb_ = 0, bloomTex_[2] = {}, bloomFbo_[2] = {}, finalTex_ = 0, finalFbo_ = 0;
};
