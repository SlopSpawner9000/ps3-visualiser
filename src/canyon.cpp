#include "canyon.h"
#include "dither.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kVertsPerQuad = 4;
constexpr int kHalvesPerVert = 12; // position, normal, texcoord: 4 halves each
constexpr int kLayers = 5;         // depth of the blur stack sampled by canyon_layer_blur

const char *kMenus[] = {"CANYON_BLUR", "CANYON_CAMERA", "CANYON_COLOUR", "CANYON_FOG", "CANYON_LINE", "CANYON_TERRAIN"};

const char *kFullscreenVs = R"(#version 330 core
layout(location = 0) in vec2 p;
out vec4 vo_tc0;
void main() {
    vo_tc0 = vec4(p * 0.5 + 0.5, 0.0, 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Glow, built around the firmware's lib/glutils/ConeFilter.fpo: an
// alpha-weighted 4-tap downsample. The vertex stage mirrors ConeFilter.vpo.
const char *kConeVs = R"(#version 330 core
layout(location = 0) in vec2 p;
uniform vec4 step;
out vec4 vo_tc0;
out vec4 vo_tc1;
void main() {
    vec2 tc = p * 0.5 + 0.5;
    vo_tc0 = vec4(tc + step.xy, tc - step.xy);
    vo_tc1 = vec4(tc + vec2(step.x, -step.y), tc - vec2(step.x, -step.y));
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Smooth (cubic B-spline) enlargement of one slice of the low-resolution blur
// stack, using four bilinear lookups.
const char *kUpsampleFs = R"(#version 330 core
uniform sampler3D tex;
uniform vec2 texSize;
uniform float z;
in vec4 vo_tc0;
out vec4 fragColor;
void main() {
    vec2 st = vo_tc0.xy * texSize - 0.5;
    vec2 i = floor(st), f = st - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 h0 = (i - 0.5 + w1 / g0) / texSize, h1 = (i + 1.5 + w3 / g1) / texSize;
    fragColor = g0.y * (g0.x * texture(tex, vec3(h0.x, h0.y, z)) + g1.x * texture(tex, vec3(h1.x, h0.y, z))) +
                g1.y * (g0.x * texture(tex, vec3(h0.x, h1.y, z)) + g1.x * texture(tex, vec3(h1.x, h1.y, z)));
}
)";

// Frame luminance, downsampled into a small mipmapped texture to get the mean.
const char *kLumFs = R"(#version 330 core
uniform sampler2D tex;
in vec4 vo_tc0;
out vec4 fragColor;
void main() {
    fragColor = vec4(dot(texture(tex, vo_tc0.xy).rgb, vec3(0.3, 0.55, 0.15)));
}
)";

// Seeds the glow: colour, with alpha = how much this pixel stands out above
// the frame's average brightness (so bright, hazy presets don't burn out).
const char *kBrightFs = R"(#version 330 core
uniform sampler2D tex;
uniform sampler2D lum;
in vec4 vo_tc0;
out vec4 fragColor;
void main() {
    vec3 c = texture(tex, vo_tc0.xy).rgb;
    float l = dot(c, vec3(0.3, 0.55, 0.15));
    float mean = textureLod(lum, vec2(0.5), 16.0).r;
    float lo = max(0.18, mean + 0.1);
    fragColor = vec4(c, smoothstep(lo, lo + 0.75, l) * (1.0 - 0.7 * clamp(mean, 0.0, 1.0)));
}
)";

// Sums the glow pyramid and screens it over the frame.
const char *kGlowFs = R"(#version 330 core
uniform sampler2D scene;
uniform sampler2D g1, g2, g3, g4, g5;
uniform float strength;
uniform float hdr; // 1: also keep the light the screen blend clips (HDR output can show it)
in vec4 vo_tc0;
out vec4 fragColor;
vec3 lvl(sampler2D t) { vec4 v = texture(t, vo_tc0.xy); return v.rgb * v.a; }
void main() {
    vec3 c = texture(scene, vo_tc0.xy).rgb;
    vec3 g = lvl(g1) * 0.08 + lvl(g2) * 0.14 + lvl(g3) * 0.24 + lvl(g4) * 0.34 + lvl(g5) * 0.42;
    g *= strength;
    vec3 sdr = 1.0 - (1.0 - c) * (1.0 - min(g, vec3(1.0)));
    fragColor = vec4(sdr + hdr * max(c + g - 1.0, vec3(0.0)), 1.0);
}
)";

void mat4Mul(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

void normalize3(float *v)
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

GLuint makeTexture(GLenum target)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(target, t);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target == GL_TEXTURE_3D)
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return t;
}

GLuint makeNoise(int size, uint32_t seed)
{
    std::vector<uint8_t> px(size_t(size) * size * 4);
    for (auto &b : px) {
        seed = seed * 1664525u + 1013904223u;
        b = uint8_t(seed >> 24);
    }
    GLuint t = makeTexture(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return t;
}

// Uncompressed 32-bit DDS with mipmaps (canyon_normalmap.dds).
GLuint loadDds(const Bytes &d)
{
    auto le32 = [&](size_t o) {
        return o + 4 <= d.size() ? uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16) |
                                       (uint32_t(d[o + 3]) << 24)
                                 : 0u;
    };
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0 || le32(88) != 32)
        return 0;
    const int h = int(le32(12)), w = int(le32(16));
    const int mips = std::max(1, int(le32(28)));
    const bool bgra = le32(92) == 0x00ff0000; // red mask
    GLuint t = makeTexture(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    size_t off = 128;
    int level = 0;
    for (int mw = w, mh = h; level < mips && off + size_t(mw) * mh * 4 <= d.size(); level++) {
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, mw, mh, 0, bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE,
                     d.data() + off);
        off += size_t(mw) * mh * 4;
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(0, level - 1));
    return t;
}

} // namespace

float Canyon::Params::get(const char *name) const
{
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == name)
            return values[i];
    return 0.0f;
}

uint16_t Canyon::toHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exp = int((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10)
            return uint16_t(sign);
        mant |= 0x800000;
        const int shift = 14 - exp;
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1)
            h++;
        return uint16_t(sign | h);
    }
    if (exp >= 31)
        return uint16_t(sign | 0x7c00);
    uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
    if (mant & 0x1000)
        h++;
    return uint16_t(h);
}

Canyon::Params Canyon::loadPreset(const std::string &name) const
{
    Params out = base_;
    if (name == kCustomName) {
        for (size_t i = 0; i < out.names.size(); i++) {
            auto it = custom_.find(out.names[i]);
            if (it != custom_.end())
                out.values[i] = it->second;
        }
        return out;
    }
    for (const char *menu : kMenus) {
        auto data = qrc_.get("override/" + name + "/canyon/" + menu + ".mnu");
        if (!data)
            continue;
        for (auto &[k, v] : parseMnu(*data)) {
            auto it = std::find(out.names.begin(), out.names.end(), k);
            if (it == out.names.end()) {
                out.names.push_back(k);
                out.values.push_back(v);
            } else {
                out.values[size_t(it - out.names.begin())] = v;
            }
        }
    }
    return out;
}

bool Canyon::init(const fs::path &devFlash, std::string &error)
{
    const fs::path qrcPath = devFlash / "vsh" / "resource" / "qgl" / "canyon.qrc";
    if (!qrc_.load(qrcPath)) {
        error = "cannot read " + qrcPath.string();
        return false;
    }
    for (const char *menu : kMenus) {
        auto data = qrc_.get(std::string("canyon/") + menu + ".mnu");
        if (!data)
            continue;
        for (auto &[k, v] : parseMnu(*data)) {
            base_.names.push_back(k);
            base_.values.push_back(v);
        }
    }
    std::set<std::string> names;
    for (auto &f : qrc_.list("override/")) {
        const auto slash = f.find('/', 9);
        if (slash != std::string::npos)
            names.insert(f.substr(9, slash - 9));
    }
    presetNames_.assign(names.begin(), names.end());
    if (presetNames_.empty()) {
        error = "canyon.qrc has no presets";
        return false;
    }
    presetNames_.push_back(kCustomName);

    auto loadShader = [&](CgShader &s, const char *path) {
        auto data = qrc_.get(path);
        if (!data || !s.parse(*data, path)) {
            error = std::string("cannot load shader ") + path;
            return false;
        }
        return true;
    };
    if (!loadShader(terrainVp_, "lib/canyon/canyon.vpo") || !loadShader(terrainFp_, "lib/canyon/canyon.fpo") ||
        !loadShader(amountVp_, "lib/canyon/canyon_blur_ammount.vpo") ||
        !loadShader(amountFp_, "lib/canyon/canyon_blur_ammount.fpo") ||
        !loadShader(blurFp_, "lib/canyon/canyon_layer_blur_blur.fpo") ||
        !loadShader(layerFp_, "lib/canyon/canyon_layer_blur.fpo"))
        return false;
    if (!loadShader(coneFp_, "lib/glutils/ConeFilter.fpo"))
        return false;
    if (!cone_.link(nullptr, kConeVs, coneFp_)) {
        error = "shader translation failed (see log)";
        return false;
    }
    brightProg_ = compileProgram(kFullscreenVs, kBrightFs, "glow seed");
    lumProg_ = compileProgram(kFullscreenVs, kLumFs, "glow luminance");
    upProg_ = compileProgram(kFullscreenVs, kUpsampleFs, "blur upsample");
    if (!lumProg_ || !upProg_) {
        error = "glow shaders failed (see log)";
        return false;
    }
    glUseProgram(brightProg_);
    glUniform1i(glGetUniformLocation(brightProg_, "tex"), 0);
    glUniform1i(glGetUniformLocation(brightProg_, "lum"), 1);
    lumTex_ = makeTexture(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 64, 64, 0, GL_RGBA, GL_FLOAT, nullptr);
    glGenerateMipmap(GL_TEXTURE_2D);
    glGenFramebuffers(1, &lumFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, lumFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lumTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glowProg_ = compileProgram(kFullscreenVs, kGlowFs, "glow composite");
    if (!brightProg_ || !glowProg_) {
        error = "glow shaders failed (see log)";
        return false;
    }
    glUseProgram(glowProg_);
    glUniform1i(glGetUniformLocation(glowProg_, "scene"), 0);
    for (int i = 1; i < 6; i++)
        glUniform1i(glGetUniformLocation(glowProg_, ("g" + std::to_string(i)).c_str()), i);
    if (!terrain_.link(&terrainVp_, nullptr, terrainFp_) || !amount_.link(&amountVp_, nullptr, amountFp_) ||
        !blur_.link(nullptr, kFullscreenVs, blurFp_) || !layer_.link(nullptr, kFullscreenVs, layerFp_)) {
        error = "shader translation failed (see log)";
        return false;
    }

    if (auto dds = qrc_.get("canyon/textures/DDS/canyon_normalmap.dds"))
        normalMap_ = loadDds(*dds);
    if (!normalMap_) {
        const uint8_t flat[4] = {128, 128, 255, 255};
        normalMap_ = makeTexture(GL_TEXTURE_2D);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, flat);
    }
    fresLut_ = makeTexture(GL_TEXTURE_2D);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 512, 1, 0, GL_RGBA, GL_FLOAT, nullptr);
    noise1_ = makeNoise(64, 1);
    noise2_ = makeNoise(128, 2);

    // Terrain: 128x128 quads of 24-byte half-float vertices, as on the PS3.
    buildMesh();
    std::vector<uint32_t> idx;
    idx.reserve(size_t(kRows) * kCols * 6);
    for (uint32_t q = 0; q < uint32_t(kRows * kCols); q++) {
        const uint32_t b = q * 4;
        for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u})
            idx.push_back(b + k);
    }
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts_.size() * 2), verts_.data(), GL_DYNAMIC_DRAW);
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
    for (int a : {0, 2, 8}) {
        glEnableVertexAttribArray(GLuint(a));
        glVertexAttribPointer(GLuint(a), 4, GL_HALF_FLOAT, GL_FALSE, 24,
                              reinterpret_cast<void *>(uintptr_t(a == 0 ? 0 : a == 2 ? 8 : 16)));
    }
    glBindVertexArray(0);

    const float tri[6] = {-1, -1, 3, -1, -1, 3};
    glGenVertexArrays(1, &quadVao_);
    glBindVertexArray(quadVao_);
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
    glBindVertexArray(0);

    auto it = std::find(presetNames_.begin(), presetNames_.end(), "canyon01");
    setPreset(it != presetNames_.end() ? int(it - presetNames_.begin()) : 0, 0.0f);

    // Fill the whole terrain before the first frame (the original fast-forwards too).
    const float silence[kCols] = {};
    for (int i = 0; i < kRows + 16; i++)
        update(1.01f / std::max(1.0f, p("POS SPEED")), silence);
    return true;
}

void Canyon::shutdown()
{
    terrain_.destroy();
    amount_.destroy();
    blur_.destroy();
    layer_.destroy();
    GLuint texs[] = {normalMap_, fresLut_, noise1_, noise2_, colorTex_, screenTex_, layers_,
                     pingTex_[0], pingTex_[1], accumTex_[0], accumTex_[1]};
    glDeleteTextures(GLsizei(std::size(texs)), texs);
    GLuint fbos[] = {colorFbo_, screenFbo_, pingFbo_[0], pingFbo_[1], accumFbo_[0], accumFbo_[1]};
    glDeleteFramebuffers(GLsizei(std::size(fbos)), fbos);
    glDeleteRenderbuffers(1, &depthRb_);
    glDeleteBuffers(1, &vbo_);
    glDeleteBuffers(1, &quadVbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteVertexArrays(1, &quadVao_);
}

void Canyon::setPreset(int index, float seconds)
{
    if (presetNames_.empty())
        return;
    index = ((index % int(presetNames_.size())) + int(presetNames_.size())) % int(presetNames_.size());
    preset_ = index;
    if (index != customIndex())
        lastBuiltin_ = index;
    to_ = loadPreset(presetNames_[size_t(index)]);
    if (seconds <= 0 || cur_.names.empty()) {
        cur_ = from_ = to_;
        morph_ = 1.0f;
        return;
    }
    from_ = cur_;
    // Align names (all presets share the base names, but be defensive).
    for (size_t i = 0; i < to_.names.size(); i++)
        if (std::find(from_.names.begin(), from_.names.end(), to_.names[i]) == from_.names.end()) {
            from_.names.push_back(to_.names[i]);
            from_.values.push_back(to_.values[i]);
        }
    morph_ = 0.0f;
    morphTime_ = seconds;
    feedbackFrames_ = 100;
}

void Canyon::setCustom(const std::map<std::string, float> &values)
{
    custom_ = values;
    if (preset_ != customIndex())
        return;
    to_ = loadPreset(kCustomName);
    if (morph_ >= 1.0f)
        cur_ = to_; // live editing: apply immediately
}

int Canyon::step(int from, int dir) const
{
    const int n = int(presetNames_.size());
    if (n <= 1)
        return from;
    int i = from;
    for (int k = 0; k < n; k++) {
        i = ((i + dir) % n + n) % n;
        if (i != customIndex() || customInRotation)
            return i;
    }
    return from;
}

std::map<std::string, float> Canyon::presetValues(int index) const
{
    std::map<std::string, float> out;
    if (index < 0 || index >= int(presetNames_.size()))
        return out;
    const Params p = loadPreset(presetNames_[size_t(index)]);
    for (size_t i = 0; i < p.names.size(); i++)
        out[p.names[i]] = p.values[i];
    return out;
}

Canyon::Stats Canyon::stats() const
{
    Stats st{};
    st.morph = morph_;
    st.scroll = scroll_;
    st.rowBase = rowBase_;
    st.peak = lastPeak_;
    st.peakEnv = peakEnv_;
    for (float v : spectrum_) {
        st.meanNorm += v / kCols;
        st.maxNorm = std::max(st.maxNorm, v);
    }
    st.lift = lift_;
    st.wantLift = wantLift_;
    st.catchUp = catchUp_;
    st.ring = ring_;
    st.feedbackFrames = feedbackFrames_;
    return st;
}

void Canyon::dumpState(std::string &out) const
{
    char buf[512];
    auto line = [&](const char *fmt, auto... args) {
        std::snprintf(buf, sizeof buf, fmt, args...);
        out += buf;
        out += '\n';
    };
    line("[visualizer]");
    line("preset = %s (index %d of %d), last builtin %s", presetNames_[size_t(preset_)].c_str(), preset_,
         int(presetNames_.size()), presetNames_[size_t(lastBuiltin_)].c_str());
    line("morph = %.3f of %.2f s, feedback frames left = %d", morph_, morphTime_, feedbackFrames_);
    line("scroll = %.3f, rowBase = %.3f, ring = %d, clock = %.2f", scroll_, rowBase_, ring_, clock_);
    line("peak = %.5g, peakEnv = %.5g", lastPeak_, peakEnv_);
    line("camera eye (rel) = %.2f %.2f %.2f, target (rel) = %.2f %.2f %.2f", camEye_[0], camEye_[1], camEye_[2],
         camTarget_[0], camTarget_[1], camTarget_[2]);
    line("lift = %.3f, want = %.3f, catchUp = %.2f", lift_, wantLift_, catchUp_);
    line("render size = %dx%d, blur div = %d (built %d), accumValid = %d", width_, height_, layerDiv(), builtDiv_,
         int(accumValid_));
    line("tweaks: wave %.2f line %.2f speed %.2f blur %.2f blurDiv %d glow %.2f fov %.2f tiles %d keepAbove %d "
         "motion %d drift %.2f driftSpeed %.2f smooth %.2f customInRotation %d",
         tweaks.waveHeight, tweaks.lineWidth, tweaks.speed, tweaks.blur, tweaks.blurDiv, tweaks.glow, tweaks.fov,
         tweaks.fallingTiles, int(tweaks.keepAboveTerrain), int(tweaks.cameraMotion), tweaks.drift,
         tweaks.driftSpeed, tweaks.cameraSmooth, int(customInRotation));
    line("[current parameter values]");
    for (size_t i = 0; i < cur_.names.size(); i++)
        line("%s = %.6g (target %.6g)", cur_.names[i].c_str(), cur_.values[i], to_.get(cur_.names[i].c_str()));
    auto row = [&](const char *name, const float *v, int n) {
        out += name;
        for (int i = 0; i < n; i++) {
            std::snprintf(buf, sizeof buf, " %.4g", v[i]);
            out += buf;
        }
        out += '\n';
    };
    line("[spectrum: 128 columns]");
    row("raw:", rawColumns_, kCols);
    row("normalised:", spectrum_, kCols);
    row("velocity:", velocity_.data(), int(velocity_.size()));
    std::vector<float> y(kCols);
    for (int i = 0; i < kCols; i++)
        y[i] = rowB_[i].pos[1];
    row("profile y (current row):", y.data(), kCols);
    float hMin = 1e9f, hMax = -1e9f, hSum = 0;
    for (float h : heights_) {
        hMin = std::min(hMin, h);
        hMax = std::max(hMax, h);
        hSum += h;
    }
    line("terrain heights (all rows): min %.4g max %.4g mean %.4g", hMin, hMax, hSum / float(heights_.size()));
    row("row z:", rowZ_.data(), int(rowZ_.size()));
}

void Canyon::stepParams(float dt)
{
    if (morph_ >= 1.0f)
        return;
    morph_ = std::min(1.0f, morph_ + dt / morphTime_);
    const float t = morph_ * morph_ * (3.0f - 2.0f * morph_);
    cur_ = to_;
    for (size_t i = 0; i < cur_.names.size(); i++)
        cur_.values[i] = from_.get(cur_.names[i].c_str()) * (1.0f - t) + to_.values[i] * t;
}

void Canyon::buildMesh()
{
    verts_.assign(size_t(kRows) * kCols * kVertsPerQuad * kHalvesPerVert, 0);
    rowZ_.assign(kRows, 0.0f);
    heights_.assign(size_t(kRows) * kCols, 0.0f);
    targets_.assign(size_t(kRows) * kCols, 0.0f);
    velocity_.assign(kCols, 0.0f);
    rowA_.assign(kCols, Row{});
    rowB_.assign(kCols, Row{});
    rowC_.assign(kCols, Row{});
    for (int i = 0; i < kCols; i++) {
        const float x = float(i) - 64.0f;
        rowA_[i] = {{x, 0, 0, 0}, {0, 1, 0, 0}};
        rowB_[i] = {{x, 0, 0.5f, 0}, {0, 1, 0, 0}};
        rowC_[i] = {{x, 0, 1, 0}, {0, 1, 0, 0}};
    }
    for (int j = 0; j < kRows; j++) {
        rowZ_[j] = float(j);
        for (int i = 0; i < kCols; i++) {
            const float curve = std::fabs(1.0f - float(i) / 64.0f);
            const float tz = float(j) - 50.0f * curve * curve;
            const float r500 = rnd() * 500.0f;
            const int corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int v = 0; v < 4; v++) {
                const size_t b = ((size_t(j) * kCols + i) * kVertsPerQuad + v) * kHalvesPerVert;
                const float ci = float(i + corner[v][0]), cj = float(j + corner[v][1]);
                const float vals[12] = {ci - 64.0f, 0, cj, 0, 0, 1, 0, rnd() * kPi, ci / 127.0f, cj / 127.0f, tz, r500};
                for (int k = 0; k < 12; k++)
                    writeHalf(b + k, vals[k]);
            }
        }
    }
    scroll_ = -10.0f;
    rowBase_ = 0.0f;
    ring_ = 0;
    vertsDirty_ = true;
}

// The XMB's preset table in custom_render_plugin (0x9c780, used when the PS3 runs
// the visualizer from the music player) overrides LINE PREFETCH: the spectrum is
// taken this many ms ahead of playback, so that a sound's terrain row has travelled
// to where the camera looks by the time it is heard.
float Canyon::prefetchSeconds() const
{
    static const std::map<std::string, int> kMs = {
        {"Bir_A01", 441}, {"Bir_A02", 441}, {"Bir_A03", 441}, {"Bir_A04", 441},
        {"Bir_B01", 441}, {"Bir_B02", 288}, {"Bir_B03", 299}, {"Bir_B04", 441},
        {"Bir_C01", 130}, {"Bir_C02", 441}, {"Bir_C03", 441}, {"Bir_C04", 441},
        {"Voyage01", 0},  {"Voyage02", 904}, {"Voyage03", 939}, {"Voyage04", 346}, {"Voyage05", 192},
        {"Voyage06", 669}, {"Voyage07", 692}, {"Voyage08", 242}, {"Voyage09", 172}, {"Voyage10", 325},
        {"Volcano01", 145}, {"Volcano02", 380}, {"Volcano03", 233}, {"Volcano04", 165},
        {"Volvic01", 0},  {"Volvic02", 0},  {"Volvic03", 0},  {"Volvic04", 0},  {"Volvic05", 0},
        {"Blue01", 270},  {"Blue02", 270},  {"Blue03", 370},  {"Blue04", 1000},
    };
    if (preset_ >= 0 && size_t(preset_) < presetNames_.size())
        if (auto it = kMs.find(presetNames_[size_t(preset_)]); it != kMs.end())
            return float(it->second) / 1000.0f;
    return std::max(0.0f, p("LINE PREFETCH")) / 1000.0f;
}

void Canyon::update(float dt, const float *columns)
{
    // Album-art colours: fade in/out over ~3 s, and glide from one album's hues to the next
    // along the shorter way round the colour wheel.
    paletteMix_ += ((havePalette_ ? 1.0f : 0.0f) - paletteMix_) * (1.0f - std::exp(-dt / 1.0f));
    if (havePalette_) {
        const float k = 1.0f - std::exp(-dt / 1.0f);
        auto glide = [k](float &from, float to) {
            const float d = std::remainder(to - from, 360.0f); // -180..180
            from = std::fmod(from + d * k + 360.0f, 360.0f);
        };
        glide(palette_.hue, paletteTarget_.hue);
        glide(palette_.accent, paletteTarget_.accent);
        palette_.sat += (paletteTarget_.sat - palette_.sat) * k;
    }
    stepParams(dt);
    updateCamera(dt);

    // Weight the spectrum towards the middle columns and normalise to <= 1.
    const float width = p("TERRAIN WIDTH");
    float peak = 0.0f;
    std::copy(columns, columns + kCols, rawColumns_);
    for (int i = 0; i < kCols; i++) {
        const float tent = 1.0f - 2.0f * std::fabs(0.5f - float(i) * (1.0f / 128.0f));
        spectrum_[i] = columns[i] * std::exp(width * tent);
        peak = std::max(peak, spectrum_[i]);
    }
    // The PS3 divides by max(1, peak) of a spectrum feed whose scaling is
    // unknown. Dividing by the instantaneous peak lets one loud bass bin
    // flatten everything else, so normalise against a slowly decaying peak
    // and lift quiet columns a little.
    // Presets boost the middle columns by up to e^25, so the peak can change by
    // many orders of magnitude between presets: never hold more than 8x the
    // current peak, or the terrain stays flat until the envelope decays.
    peakEnv_ = std::min(std::max(peak, peakEnv_ * std::exp(-dt / 3.0f)), peak * 8.0f);
    lastPeak_ = peak;
    const float norm = std::max(peakEnv_, 0.05f);
    static const float shape = [] {
        const char *e = SDL_getenv("PS3VIS_SHAPE");
        return e ? float(std::atof(e)) : 0.6f;
    }();
    // PS3VIS_NORM=ps3: the original's spec / max(1, peak), nothing else (testing).
    const bool ps3Norm = tweaks.spectrumMode != 2;
    // PS3VIS_NORM=hybrid (testing): level the raw columns against a slow per-song
    // envelope, scale by PS3VIS_G, then the original tent boost and / max(1, peak).
    static const bool hybrid = SDL_getenv("PS3VIS_NORM") && !std::strcmp(SDL_getenv("PS3VIS_NORM"), "hybrid");
    static const float hg = SDL_getenv("PS3VIS_G") ? float(std::atof(SDL_getenv("PS3VIS_G"))) : 0.05f;
    static const float hdb = SDL_getenv("PS3VIS_DB") ? float(std::atof(SDL_getenv("PS3VIS_DB"))) : 0.0f;
    static const bool hcol = SDL_getenv("PS3VIS_COLENV") != nullptr;
    static const float hshape = SDL_getenv("PS3VIS_SHAPE") ? float(std::atof(SDL_getenv("PS3VIS_SHAPE"))) : 1.0f;
    if (hybrid) {
        float rawPeak = 0.0f;
        for (int i = 0; i < kCols; i++)
            rawPeak = std::max(rawPeak, columns[i]);
        rawEnv_ = std::max(rawPeak, rawEnv_ * std::exp(-dt / 10.0f));
        const float rn = std::max(rawEnv_, 0.02f);
        float m = 1.0f;
        for (int i = 0; i < kCols; i++) {
            const float tent = 1.0f - 2.0f * std::fabs(0.5f - float(i) * (1.0f / 128.0f));
            float x = columns[i] / rn;
            if (hcol) { // PS3VIS_COLENV=1: each column against its own slow envelope (floored at 10% of the song's)
                colEnv_[i] = std::max(columns[i], colEnv_[i] * std::exp(-dt / 10.0f));
                x = columns[i] / std::max(colEnv_[i], 0.1f * rn);
            }
            if (hdb > 0) // PS3VIS_DB=range: decibel feed, 0 at -range dB below the envelope, 1 at it
                x = std::max(0.0f, 1.0f + 20.0f * std::log10(std::max(x, 1e-6f)) / hdb);
            spectrum_[i] = hg * x * std::exp(width * tent);
            m = std::max(m, spectrum_[i]);
        }
        for (float &s : spectrum_)
            s = std::pow(s / m, hshape);
    } else if (ps3Norm) {
        float m = std::max(1.0f, peak);
        if (tweaks.spectrumMode == 1) {
            // Smooth bass: the divisor follows noisy boosted mid columns and would
            // rescale steady bass every frame, so average it over ~0.1 s (and clamp,
            // since a smoothed divisor can be briefly below the peak).
            peakSmooth_ += (m - peakSmooth_) * (peakSmooth_ > 0 ? 1.0f - std::exp(-dt / 0.1f) : 1.0f);
            m = peakSmooth_;
        }
        for (float &s : spectrum_)
            s = std::min(s / m, 1.0f);
    } else {
        for (float &s : spectrum_)
            s = std::pow(std::min(s / norm, 1.0f), shape);
    }
    // PS3VIS_DUMP_SPECTRUM=file: append the normalised 128 columns every update (testing).
    if (static FILE *dump = SDL_getenv("PS3VIS_DUMP_SPECTRUM") ? std::fopen(SDL_getenv("PS3VIS_DUMP_SPECTRUM"), "w")
                                                               : nullptr;
        dump) {
        for (int i = 0; i < kCols; i++)
            std::fprintf(dump, i ? " %.4f" : "%.4f", spectrum_[i]);
        std::fprintf(dump, "\n");
    }
    if (static const bool dbg = SDL_getenv("PS3VIS_DEBUG_SPECTRUM") != nullptr; dbg) {
        static int n = 0;
        if (++n % 60 == 0) {
            float raw = 0, mean = 0;
            for (int i = 0; i < kCols; i++) {
                raw = std::max(raw, columns[i]);
                mean += spectrum_[i] / kCols;
            }
            SDL_Log("spectrum raw max %.4f  peak %.3f  normalised mean %.3f  edge %.3f mid %.3f", raw, peak, mean,
                    spectrum_[2], spectrum_[60]);
        }
    }

    scroll_ += dt * p("POS SPEED") * tweaks.speed;
    // The PS3 adds at most one row per update, which only keeps up while
    // POS SPEED x frame time < 1 row; slower frames (or fast presets such as
    // canyon10) leave the terrain permanently behind the camera. Catch up
    // fully instead, and resync if a whole terrain length was skipped.
    if (scroll_ - rowBase_ > float(kRows))
        rowBase_ = std::floor(scroll_) - float(kRows);
    while (rowBase_ < scroll_) {
        rowBase_ += 1.0f;
        generateRow();
    }
    if (scroll_ > 99.0f)
        rebase(scroll_);
}

void Canyon::updateCamera(float dt)
{
    // Camera drift (not from the firmware): the camera slides slowly left and
    // right (eye and target together, so the view direction never changes),
    // from two incommensurate sines so the motion never loops.
    if (tweaks.cameraMotion)
        clock_ += dt * tweaks.driftSpeed;
    const float tau = 2.0f * kPi, tt = clock_;
    const float amount = tweaks.cameraMotion ? tweaks.drift : 0.0f;
    const float slide = amount * (14.0f * std::sin(tau * tt / 47.0f) + 4.0f * std::sin(tau * tt / 19.0f + 1.3f));
    float eye[3] = {p("POS X") + slide, p("POS Y"), p("POS Z")};
    float target[3] = {p("TARGET X") + slide, p("TARGET Y"), p("TARGET Z")};

    // Keep the camera above the terrain (not in the original): low cameras such
    // as Voyage03's can end up inside tall hills. Look at the terrain around and
    // ahead of the camera, then rise early and settle slowly, so it dodges hills
    // instead of riding every wave.
    float wantLift = 0.0f;
    if (tweaks.keepAboveTerrain && !heights_.empty()) {
        const float zAbs = eye[2] + scroll_;
        const float sx = std::fabs(p("SCALE X")) > 1e-3f ? p("SCALE X") : 1.0f;
        const int col = int(std::lround(eye[0] / sx - (p("TERRAIN SLIDE") * -0.25f - 64.0f)));
        // Final height as canyon.vpo computes it (ignoring tile scatter):
        //   w  = line proximity = 1 - smoothstep(|z - lineZ| / LINE WIDTH)
        //   hF = y * (Sy + LINE HEIGHT * cos(|lineZ - z| * LINE LENGTH) * (LINE BIAS + w))
        //   h  = hF + 0.2 * w * (target * Sy - hF)
        const float lineZ = p("LINE AHEAD") + scroll_;
        const float lw = std::max(1e-3f, std::fabs(p("LINE WIDTH") * tweaks.lineWidth));
        const float sy = 0.3f * p("TERRAIN HEIGHT") * tweaks.waveHeight;
        const float ly = p("LINE HEIGHT") * tweaks.waveHeight, lz = p("LINE LENGTH"), lb = p("LINE BIAS");
        float top = -1e9f;
        for (int r = 0; r < kRows; r++) {
            const float z = rowZ_[r] + 0.5f;
            const float ahead = z - zAbs;
            if (ahead < -4.0f || ahead > 25.0f)
                continue;
            const float a = std::clamp(std::fabs(z - lineZ) / lw, 0.0f, 1.0f);
            const float w = 1.0f - a * a * (3.0f - 2.0f * a);
            const float f = sy + ly * std::cos(std::fabs(lineZ - z) * lz) * (lb + w);
            for (int dc = -4; dc <= 4; dc++) {
                const size_t i = size_t(r) * kCols + size_t(std::clamp(col + dc, 0, kCols - 1));
                const float hF = heights_[i] * f;
                top = std::max(top, hF + 0.2f * w * (targets_[i] * sy - hF));
            }
        }
        wantLift = std::max(0.0f, top + 5.0f - eye[1]);
    }
    // Right after a preset change finishes, catch up quickly in both directions
    // for 3 s (morphs can swing the terrain scale a lot, and rows built under
    // the old preset keep passing by for a moment), then go back to dodging
    // gently. A large overshoot also comes down quickly at any time.
    if (morphWas_ < 1.0f && morph_ >= 1.0f)
        catchUp_ = 3.0f;
    morphWas_ = morph_;
    catchUp_ = std::max(0.0f, catchUp_ - dt);
    float tauLift = wantLift > lift_ ? 0.5f : (lift_ - wantLift > 8.0f ? 0.8f : 4.0f);
    if (catchUp_ > 0.0f)
        tauLift = 0.3f;
    // Never more than 30 units: presets such as Bir_B01 deliberately put the
    // camera among tall valley walls, and chasing those sends it into the sky.
    wantLift = std::min(wantLift, 30.0f);
    wantLift_ = wantLift;
    lift_ += (wantLift - lift_) * (1.0f - std::exp(-dt / tauLift));
    // Raise only the eye: the target stays on the terrain, so the camera tilts
    // down to keep the scene in view (lifting both aimed it over the far wall).
    eye[1] += lift_;
    if (static const bool dbg = SDL_getenv("PS3VIS_DEBUG_CAMERA") != nullptr; dbg)
        SDL_Log("camera lift want %.2f have %.2f", wantLift, lift_);
    // Ease towards the goal; the original eases its camera by 10% per frame.
    const float k = (!camValid_ || tweaks.cameraSmooth <= 0.0f) ? 1.0f : 1.0f - std::exp(-dt / tweaks.cameraSmooth);
    for (int i = 0; i < 3; i++) {
        camEye_[i] += (eye[i] - camEye_[i]) * k;
        camTarget_[i] += (target[i] - camTarget_[i]) * k;
    }
    camValid_ = true;
}

void Canyon::generateRow()
{
    const float slide = p("TERRAIN SLIDE") * -0.25f - 64.0f;
    const float vCentre = p("VALLEY CENTRE"), vWidth = p("VALLEY WIDTH"), vSharp = p("VALLEY SHARP");
    const float vHeight = p("VALLEY HEIGHT");

    for (int i = 0; i < kCols; i++) {
        const float x = float(i) / 64.0f - 1.0f;
        const float c = std::clamp((std::fabs(x - vCentre) - vWidth) * vSharp, 0.0f, 1.0f);
        float target = c * c * (3.0f - 2.0f * c) * vHeight + spectrum_[i];
        target += (rnd() * 0.5f + 0.5f) * 0.02f;
        const float k = 0.7f + std::fabs(x * 0.295f);
        velocity_[i] += (target - rowB_[i].pos[1]) * (1.0f - k);
        rowC_[i].pos[0] = float(i) + slide;
        rowC_[i].pos[1] = rowB_[i].pos[1] + velocity_[i];
        rowC_[i].pos[2] = 1.0f;
        rowC_[i].pos[3] = target;
        rowA_[i].pos[2] = 0.0f;
        velocity_[i] *= k * 0.5f;
    }
    // Five relaxation passes across the new profile.
    for (int pass = 0; pass < 5; pass++)
        for (int i = 0; i < kCols; i++) {
            const float l = rowC_[std::max(i - 1, 0)].pos[1], r = rowC_[std::min(i + 1, kCols - 1)].pos[1];
            float &y = rowC_[i].pos[1];
            y += (l + r - 2.0f * y) * 0.25f;
        }
    // Normals of the middle profile from its neighbours across and along.
    for (int i = 0; i < kCols; i++) {
        const Row &l = rowB_[std::max(i - 1, 0)], &r = rowB_[std::min(i + 1, kCols - 1)];
        const float a[3] = {l.pos[0] - r.pos[0], l.pos[1] - r.pos[1], l.pos[2] - r.pos[2]};
        const float b[3] = {rowC_[i].pos[0] - rowA_[i].pos[0], rowC_[i].pos[1] - rowA_[i].pos[1],
                            rowC_[i].pos[2] - rowA_[i].pos[2]};
        float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        normalize3(n);
        std::memcpy(rowB_[i].nrm, n, sizeof n);
        rowB_[i].nrm[3] = 0.0f;
    }

    // Emit the oldest profile as the newest strip of quads in the ring.
    const float zNear = rowBase_ + 127.0f, zFar = rowBase_ + 128.0f;
    rowZ_[ring_] = zNear;
    for (int i = 0; i < kCols; i++) {
        heights_[size_t(ring_) * kCols + size_t(i)] = rowA_[i].pos[1];
        targets_[size_t(ring_) * kCols + size_t(i)] = rowA_[i].pos[3];
    }
    for (int i = 0; i < kCols; i++) {
        const int i1 = std::min(i + 1, kCols - 1);
        const float curve = std::fabs(1.0f - float(i) / 64.0f);
        const float tz = zFar - 50.0f * curve * curve;
        const float r500 = rnd() * 500.0f;
        const Row *src[4] = {&rowA_[i], &rowA_[i1], &rowA_[i1], &rowA_[i]};
        const float z[4] = {zNear, zNear, zFar, zFar};
        for (int v = 0; v < 4; v++) {
            const size_t b = ((size_t(ring_) * kCols + i) * kVertsPerQuad + v) * kHalvesPerVert;
            writeHalf(b + 0, src[v]->pos[0]);
            writeHalf(b + 1, src[v]->pos[1]);
            writeHalf(b + 2, z[v]);
            writeHalf(b + 3, src[v]->pos[3]);
            writeHalf(b + 4, src[v]->nrm[0]);
            writeHalf(b + 5, src[v]->nrm[1]);
            writeHalf(b + 6, src[v]->nrm[2]);
            writeHalf(b + 10, tz);
            writeHalf(b + 11, r500);
        }
    }
    // On the PS3 every strip is flat along z, which leaves hairline cracks
    // wherever the next strip is higher. Stitch the previous strip's far edge
    // to this profile so the surface is continuous.
    const int prevStrip = (ring_ + kRows - 1) % kRows;
    for (int i = 0; i < kCols; i++) {
        const int i1 = std::min(i + 1, kCols - 1);
        const Row *src[2] = {&rowA_[i1], &rowA_[i]}; // far corners v2, v3
        for (int v = 2; v < 4; v++) {
            const size_t b = ((size_t(prevStrip) * kCols + i) * kVertsPerQuad + v) * kHalvesPerVert;
            const Row *r = src[v - 2];
            writeHalf(b + 0, r->pos[0]);
            writeHalf(b + 1, r->pos[1]);
            writeHalf(b + 3, r->pos[3]);
            writeHalf(b + 4, r->nrm[0]);
            writeHalf(b + 5, r->nrm[1]);
            writeHalf(b + 6, r->nrm[2]);
        }
    }

    dirtyStrips_.push_back(ring_);
    dirtyStrips_.push_back(prevStrip);
    rowA_ = rowB_;
    rowB_ = rowC_;
    ring_ = (ring_ + 1) % kRows;
}

void Canyon::rebase(float delta)
{
    rowBase_ -= delta;
    scroll_ -= delta;
    for (int j = 0; j < kRows; j++) {
        rowZ_[j] -= delta;
        for (int i = 0; i < kCols; i++) {
            const float curve = std::fabs(1.0f - float(i) / 64.0f);
            const float tz = rowZ_[j] + 1.0f - 50.0f * curve * curve;
            for (int v = 0; v < 4; v++) {
                const size_t b = ((size_t(j) * kCols + i) * kVertsPerQuad + v) * kHalvesPerVert;
                writeHalf(b + 2, v < 2 ? rowZ_[j] : rowZ_[j] + 1.0f);
                writeHalf(b + 10, tz);
            }
        }
    }
    vertsDirty_ = true;
}

namespace {
// Moves a colour's hue to `hue` (degrees), with at least `minSat` saturation and the same
// brightness, blended by `mix`.
void tint(float *rgb, float hue, float minSat, float mix)
{
    if (mix <= 0.0f)
        return;
    const float r = std::max(rgb[0], 0.0f), g = std::max(rgb[1], 0.0f), b = std::max(rgb[2], 0.0f);
    const float v = std::max({r, g, b});
    if (v <= 0.0f)
        return;
    const float s = std::max((v - std::min({r, g, b})) / v, minSat);
    const float h = std::fmod(hue, 360.0f) / 60.0f, f = h - std::floor(h);
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float out[3];
    switch (int(h) % 6) {
    case 0: out[0] = v, out[1] = t, out[2] = p; break;
    case 1: out[0] = q, out[1] = v, out[2] = p; break;
    case 2: out[0] = p, out[1] = v, out[2] = t; break;
    case 3: out[0] = p, out[1] = q, out[2] = v; break;
    case 4: out[0] = t, out[1] = p, out[2] = v; break;
    default: out[0] = v, out[1] = p, out[2] = q; break;
    }
    for (int c = 0; c < 3; c++)
        rgb[c] += (out[c] - rgb[c]) * mix;
}
} // namespace

void Canyon::sceneColours(float fog[3], float line[3]) const
{
    for (int k = 0; k < 3; k++) {
        static const char *fogNames[3] = {"FOG R", "FOG G", "FOG B"}, *lineNames[3] = {"LINE R", "LINE G", "LINE B"};
        fog[k] = p(fogNames[k]);
        line[k] = p(lineNames[k]);
    }
    tint(fog, palette_.hue, palette_.sat * 0.5f, paletteMix_);
    tint(line, palette_.accent, palette_.sat * 0.6f, paletteMix_);
}

void Canyon::setPalette(const AlbumPalette *palette)
{
    havePalette_ = palette != nullptr;
    if (!palette)
        return;
    paletteTarget_ = *palette;
    if (paletteMix_ < 0.02f) // coming from the preset's own colours: just fade in
        palette_ = *palette;
}

void Canyon::updateFresLut()
{
    // Rim-light ramp: per channel pow(1 - u, COLOUR x) * SCALE + BIAS.
    const float fade = p("FADE COLOUR") * p("FADE COLOUR");
    const float e[3] = {p("COLOUR R"), p("COLOUR G"), p("COLOUR B")};
    const float scale = p("COLOUR SCALE"), bias = p("COLOUR BIAS");
    std::vector<uint16_t> px(512 * 4);
    for (int k = 0; k < 512; k++) {
        const float x = 1.0f - float(k) / 511.0f;
        float rgb[3];
        for (int c = 0; c < 3; c++)
            rgb[c] = (std::pow(x, e[c]) * scale + bias) * fade;
        tint(rgb, palette_.hue, palette_.sat * 0.7f, paletteMix_);
        for (int c = 0; c < 3; c++)
            px[k * 4 + c] = toHalf(rgb[c]);
        px[k * 4 + 3] = toHalf(1.0f);
    }
    glBindTexture(GL_TEXTURE_2D, fresLut_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 1, GL_RGBA, GL_HALF_FLOAT, px.data());
}

int Canyon::layerDiv() const
{
    // The blur stack runs at 1/div of the window; PS3VIS_LAYER_DIV overrides it.
    static const char *env = SDL_getenv("PS3VIS_LAYER_DIV");
    if (env)
        return std::max(1, std::atoi(env));
    return tweaks.blurHq ? 1 : std::clamp(tweaks.blurDiv, 1, 8); // high quality: full-resolution stack
}

void Canyon::resize(int w, int h)
{
    if (w == width_ && h == height_ && layerDiv() == builtDiv_)
        return;
    builtDiv_ = layerDiv();
    width_ = w;
    height_ = h;
    accumValid_ = false;
    auto texture2D = [](GLuint &t, int tw, int th, GLenum fmt) {
        if (!t)
            t = makeTexture(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(fmt), tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    };
    auto fbo = [](GLuint &f, GLuint tex, GLuint depth) {
        if (!f)
            glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (depth)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "incomplete framebuffer");
    };
    if (!depthRb_)
        glGenRenderbuffers(1, &depthRb_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    texture2D(colorTex_, w, h, GL_RGBA16F);
    // Mipmapped, so blur passes at low resolution average instead of skipping pixels.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    texture2D(screenTex_, w, h, GL_RGBA8);
    fbo(colorFbo_, colorTex_, depthRb_);
    fbo(screenFbo_, screenTex_, depthRb_);
    const int lw = std::max(1, w / layerDiv()), lh = std::max(1, h / layerDiv());
    for (int i = 0; i < 2; i++) {
        texture2D(pingTex_[i], lw, lh, GL_RGBA16F);
        fbo(pingFbo_[i], pingTex_[i], 0);
        texture2D(accumTex_[i], w, h, GL_RGBA16F);
        fbo(accumFbo_[i], accumTex_[i], 0);
    }
    texture2D(finalTex_, w, h, GL_RGBA16F); // float: HDR highlights may exceed 1
    fbo(finalFbo_, finalTex_, 0);
    for (int i = 0, gw = w / 2, gh = h / 2; i < kGlowLevels; i++, gw /= 2, gh /= 2) {
        texture2D(glowTex_[i], std::max(1, gw), std::max(1, gh), GL_RGBA16F);
        fbo(glowFbo_[i], glowTex_[i], 0);
    }
    if (!layers_)
        layers_ = makeTexture(GL_TEXTURE_3D);
    glBindTexture(GL_TEXTURE_3D, layers_);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, lw, lh, kLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // Below half resolution the stack is enlarged smoothly into a half-res copy.
    if (layerDiv() > 2) {
        const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
        if (!layersHi_)
            layersHi_ = makeTexture(GL_TEXTURE_3D);
        glBindTexture(GL_TEXTURE_3D, layersHi_);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, hw, hh, kLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        texture2D(hiTex_, hw, hh, GL_RGBA16F);
        fbo(hiFbo_, hiTex_, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Canyon::drawTerrain()
{
    // The slot about to be overwritten (oldest) and the newest strip are skipped.
    glBindVertexArray(vao_);
    const int r = ring_;
    if (r <= 126)
        glDrawElements(GL_TRIANGLES, (127 - r) * kCols * 6, GL_UNSIGNED_INT,
                       reinterpret_cast<void *>(uintptr_t(size_t(r + 1) * kCols * 6 * 4)));
    if (r >= 2)
        glDrawElements(GL_TRIANGLES, (r - 1) * kCols * 6, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void Canyon::render(int width, int height)
{
    resize(width, height);
    updateFresLut();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (vertsDirty_) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(verts_.size() * 2), verts_.data());
    } else {
        const size_t strip = size_t(kCols) * kVertsPerQuad * kHalvesPerVert;
        for (int r : dirtyStrips_)
            glBufferSubData(GL_ARRAY_BUFFER, GLintptr(r * strip * 2), GLsizeiptr(strip * 2), verts_.data() + r * strip);
    }
    vertsDirty_ = false;
    dirtyStrips_.clear();

    const float s = scroll_;
    const float fade = p("FADE COLOUR") * p("FADE COLOUR");
    float fog[3] = {p("FOG R") * fade, p("FOG G") * fade, p("FOG B") * fade};
    float lineColour[3] = {p("LINE R") * fade, p("LINE G") * fade, p("LINE B") * fade};
    tint(fog, palette_.hue, palette_.sat * 0.5f, paletteMix_);
    tint(lineColour, palette_.accent, palette_.sat * 0.6f, paletteMix_);

    // Camera: positions scroll along +z with the terrain.
    const float eye[3] = {camEye_[0], camEye_[1], camEye_[2] + s};
    const float target[3] = {camTarget_[0], camTarget_[1], camTarget_[2] + s};
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalize3(f);
    float side[3] = {f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0}; // f x up(0,1,0)
    normalize3(side);
    const float up[3] = {side[1] * f[2] - side[2] * f[1], side[2] * f[0] - side[0] * f[2],
                         side[0] * f[1] - side[1] * f[0]};
    const float view[16] = {side[0], side[1], side[2], -(side[0] * eye[0] + side[1] * eye[1] + side[2] * eye[2]),
                            up[0],   up[1],   up[2],   -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]),
                            -f[0],   -f[1],   -f[2],   f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2],
                            0,       0,       0,       1};
    const float zn = 0.1f, zf = 10000.0f;
    // ZOOM is the vertical field of view of the PS3's 16:9 frame. Keep that
    // framing on other window shapes: wider windows crop top and bottom.
    const float aspect = float(width) / float(std::max(1, height));
    float halfTan = std::tan(p("ZOOM") * (kPi / 180.0f) * 0.5f) * tweaks.fov;
    if (aspect > 16.0f / 9.0f)
        halfTan *= (16.0f / 9.0f) / aspect;
    const float ft = 1.0f / halfTan;
    const float proj[16] = {ft / aspect, 0, 0, 0, 0, ft, 0, 0, 0, 0, (zf + zn) / (zn - zf), 2 * zf * zn / (zn - zf),
                            0, 0, -1, 0};
    float mvp[16];
    mat4Mul(proj, view, mvp);

    const float line[4] = {p("LINE WIDTH") * tweaks.lineWidth, p("LINE HEIGHT") * tweaks.waveHeight,
                           p("LINE LENGTH"), p("LINE BIAS")};
    // TIME AHEAD below the far edge (~128 rows) makes the far terrain fly apart
    // into tiles that drop into place as they approach; 200 disables that.
    // Auto keeps it for the presets built from tiles (TIME AHEAD < 100: the
    // canyon12/13 and Voyage01/11 warps, Volvic, Bir_B01's swarm) and drops it
    // where it only scatters the horizon (~115-121: Bir_A/C, Voyage04, ...).
    const bool tiles = tweaks.fallingTiles == 2 || (tweaks.fallingTiles == 1 && p("TIME AHEAD") < 100.0f);
    const float timeAhead = tiles ? p("TIME AHEAD") : std::max(p("TIME AHEAD"), 200.0f);
    const float time[4] = {p("LINE AHEAD") + s, timeAhead + s, 126.9f + s, s};
    const float scaleY = 0.5f * p("TERRAIN HEIGHT") * tweaks.waveHeight * 0.3f * (1.0f + std::cos(0.0f));

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    // 1. Terrain colour.
    glBindFramebuffer(GL_FRAMEBUFFER, colorFbo_);
    glClearColor(fog[0], fog[1], fog[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    terrain_.use();
    terrain_.set("_Line", line[0], line[1], line[2], line[3]);
    terrain_.set("_Time", time[0], time[1], time[2], time[3]);
    terrain_.set("_TexScale", p("TEXTURE SCALE"));
    terrain_.set("_Fog_min", p("FOG MIN"));
    terrain_.set("_Fog_max_min", p("FOG MAX") - p("FOG MIN"));
    terrain_.set("_Scale", p("SCALE X"), scaleY, 1.0f, 0.0f);
    terrain_.set("_Eye", eye[0], eye[1], eye[2], 1.0f);
    terrain_.setMatrix("_MVM", mvp);
    terrain_.set("_FogColour", fog[0], fog[1], fog[2], 0.0f);
    terrain_.set("_LineColour", lineColour[0], lineColour[1], lineColour[2], 0.0f);
    terrain_.set("_BumpScale", p("BUMP SCALE"));
    terrain_.setTexture("_NormalMap", GL_TEXTURE_2D, normalMap_);
    terrain_.setTexture("_FresLUT", GL_TEXTURE_2D, fresLut_);
    terrain_.upload();
    drawTerrain();

    // 2. Per-pixel blur amount from distance to the focal plane.
    glBindFramebuffer(GL_FRAMEBUFFER, screenFbo_);
    glClearColor(p("SKY BLUR"), 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    amount_.use();
    amount_.set("_Line", line[0], line[1], line[2], line[3]);
    amount_.set("_Time", time[0], time[1], time[2], time[3]);
    amount_.set("_Focal_depth", p("FOCAL DEPTH"));
    amount_.set("_Focus", tweaks.blur / (p("FOCAL VARIANCE") != 0.0f ? p("FOCAL VARIANCE") : 1.0f));
    amount_.set("_Scale", p("SCALE X"), scaleY, 1.0f, 0.0f);
    amount_.set("_Eye", eye[0], eye[1], eye[2], 1.0f);
    amount_.setMatrix("_MVM", mvp);
    amount_.upload();
    drawTerrain();

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(quadVao_);

    // 3. Stack of progressively blurred copies (3D texture, one layer per pass).
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glGenerateMipmap(GL_TEXTURE_2D);
    const int lw = std::max(1, width / layerDiv()), lh = std::max(1, height / layerDiv());
    glViewport(0, 0, lw, lh);
    blur_.use();
    // Each pass averages 4 diagonal taps one source texel away (variance = step^2 per axis). At
    // 1/D resolution a pass spreads over D screen pixels. High quality keeps that blur size at
    // full resolution: several passes per layer whose variances add up to the low-res one
    // (layer 0: the D x D downsample, D^2/12, plus one texel), with steps of at most sqrt(2)
    // texels so no pixels are skipped.
    const int refDiv = std::clamp(tweaks.blurDiv, 1, 8);
    const bool hq = tweaks.blurHq && layerDiv() == 1 && refDiv > 1;
    int last = -1; // ping texture holding the latest result (-1: the sharp frame)
    for (int i = 0; i < kLayers; i++) {
        const float variance = !hq ? 1.0f : i == 0 ? float(refDiv * refDiv) / 12.0f + 1.0f : float(refDiv * refDiv);
        const int passes = hq ? std::max(1, int(std::ceil(variance / 2.0f))) : 1;
        const float step = std::sqrt(variance / float(passes));
        for (int k = 0; k < passes; k++) {
            const GLuint src = last < 0 ? colorTex_ : pingTex_[last];
            const int sw = last < 0 ? width : lw, sh = last < 0 ? height : lh;
            const int dst = last < 0 ? (i & 1) : last ^ 1;
            glBindFramebuffer(GL_FRAMEBUFFER, pingFbo_[dst]);
            blur_.set("_Step", step / float(sw), step / float(sh));
            blur_.setTexture("_Tex", GL_TEXTURE_2D, src);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            last = dst;
        }
        glBindTexture(GL_TEXTURE_3D, layers_);
        glCopyTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, i, 0, 0, lw, lh);
    }
    const bool smoothUp = layerDiv() > 2;
    if (smoothUp) {
        const int hw = std::max(1, width / 2), hh = std::max(1, height / 2);
        glViewport(0, 0, hw, hh);
        glUseProgram(upProg_);
        glUniform1i(glGetUniformLocation(upProg_, "tex"), 0);
        glUniform2f(glGetUniformLocation(upProg_, "texSize"), float(lw), float(lh));
        glActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < kLayers; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, hiFbo_);
            glBindTexture(GL_TEXTURE_3D, layers_);
            glUniform1f(glGetUniformLocation(upProg_, "z"), (float(i) + 0.5f) / float(kLayers));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindTexture(GL_TEXTURE_3D, layersHi_);
            glCopyTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, i, 0, 0, hw, hh);
        }
    }

    // 4. Depth-of-field + feedback composite over the previous frame.
    const int prev = accum_, cur = accum_ ^ 1;
    if (!accumValid_) {
        glBindFramebuffer(GL_FRAMEBUFFER, accumFbo_[prev]);
        glViewport(0, 0, width, height);
        glClearColor(fog[0], fog[1], fog[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        accumValid_ = true;
    }
    // The composite blends the blurred layers over the sharp frame; the
    // previous output only feeds in through _Front while _Feedback fades out.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, colorFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, accumFbo_[cur]);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, accumFbo_[cur]);
    glViewport(0, 0, width, height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    layer_.use();
    layer_.set("_LOD", p("LOD") * tweaks.blur);
    // Feedback runs at full strength while the visualizer is on: the 100-frame ramp
    // to 0 at 0xd36c is the shut-down fade (0xaa20), not a start-up one.
    // PS3VIS_FEEDBACK_FADE=1 restores the old (wrong) start-up fade for comparison.
    static const bool oldFade = SDL_getenv("PS3VIS_FEEDBACK_FADE") != nullptr;
    layer_.set("_Feedback", p("FEEDBACK") * (oldFade ? float(feedbackFrames_) / 100.0f : 1.0f));
    if (feedbackFrames_ > 0)
        feedbackFrames_--;
    layer_.set("_ResRecip", 1.0f / float(width), 1.0f / float(height));
    layer_.set("_NoiseUV", rnd() * 0.5f + 0.5f, rnd() * 0.5f + 0.5f, 1.0f / 128.0f, 0.0f);
    layer_.set("_FeedbackScale", p("FEEDBACK X"), p("FEEDBACK Y"));
    layer_.set("_FeedbackRot", std::sin(p("FEEDBACK ROT")), std::cos(p("FEEDBACK ROT")));
    layer_.setTexture("_Screen", GL_TEXTURE_2D, screenTex_);
    // PS3VIS_FRONT=final (testing): feed back last frame's displayed image (after glow).
    static const bool frontFinal = SDL_getenv("PS3VIS_FRONT") && !std::strcmp(SDL_getenv("PS3VIS_FRONT"), "final");
    // PS3VIS_FRONT=color (testing): this frame's sharp image instead of the previous output.
    static const bool frontColor = SDL_getenv("PS3VIS_FRONT") && !std::strcmp(SDL_getenv("PS3VIS_FRONT"), "color");
    layer_.setTexture("_Front", GL_TEXTURE_2D,
                      frontColor ? colorTex_ : frontFinal && tweaks.glow > 0.0f ? finalTex_ : accumTex_[prev]);
    layer_.setTexture("_Noise1", GL_TEXTURE_2D, noise1_);
    layer_.setTexture("_Tex", GL_TEXTURE_3D, smoothUp ? layersHi_ : layers_);
    layer_.setTexture("_Noise2", GL_TEXTURE_2D, noise2_);
    // The PS3 samples these two upside down relative to window coordinates.
    layer_.setFlipY("_Tex", true);
    layer_.setFlipY("_Front", true);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    accum_ = cur;
    renderGlow(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Canyon::renderGlow(int w, int h)
{
    if (tweaks.glow <= 0.0f)
        return;
    glDisable(GL_BLEND);
    glBindVertexArray(quadVao_);
    // Average brightness of the frame.
    glBindFramebuffer(GL_FRAMEBUFFER, lumFbo_);
    glViewport(0, 0, 64, 64);
    glUseProgram(lumProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accumTex_[accum_]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, lumTex_);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Seed at half resolution, then halve with ConeFilter for each level.
    int lw = std::max(1, w / 2), lh = std::max(1, h / 2);
    glBindFramebuffer(GL_FRAMEBUFFER, glowFbo_[0]);
    glViewport(0, 0, lw, lh);
    glUseProgram(brightProg_);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, lumTex_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accumTex_[accum_]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    cone_.use();
    const GLint stepLoc = glGetUniformLocation(cone_.id(), "step");
    for (int i = 1; i < kGlowLevels; i++) {
        const int sw = lw, sh = lh;
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
        glBindFramebuffer(GL_FRAMEBUFFER, glowFbo_[i]);
        glViewport(0, 0, lw, lh);
        glUniform4f(stepLoc, 1.0f / float(sw), 1.0f / float(sh), 0, 0);
        cone_.setTexture("_Tex", GL_TEXTURE_2D, glowTex_[i - 1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, finalFbo_);
    glViewport(0, 0, w, h);
    glUseProgram(glowProg_);
    glUniform1f(glGetUniformLocation(glowProg_, "strength"), tweaks.glow);
    glUniform1f(glGetUniformLocation(glowProg_, "hdr"), hdrOutput ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accumTex_[accum_]);
    for (int i = 1; i < 6; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, glowTex_[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void Canyon::present(int width, int height, GLuint targetFbo, float dither)
{
    // (Rendering at another size than the window, e.g. the render-scale stress test, is scaled.)
    if (!accumValid_) {
        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    // PS3VIS_DEBUG=color|amount shows an intermediate pass instead.
    static const char *debug = SDL_getenv("PS3VIS_DEBUG");
    GLuint src = tweaks.glow > 0.0f ? finalFbo_ : accumFbo_[accum_];
    if (debug && !std::strcmp(debug, "color"))
        src = colorFbo_;
    else if (debug && !std::strcmp(debug, "amount"))
        src = screenFbo_;
    if (dither > 0 && !debug) {
        dither::copy(tweaks.glow > 0.0f ? finalTex_ : accumTex_[accum_], width_, height_, targetFbo, width, height, dither,
                     hdrOutput);
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    const bool same = width == width_ && height == height_;
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width, height, GL_COLOR_BUFFER_BIT, same ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
}
