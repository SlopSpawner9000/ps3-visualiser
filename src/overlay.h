// Text and simple shapes drawn over the visualizer (track info, OSD, help).
#pragma once

#include "firmware.h"
#include "gl.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct stbtt_fontinfo;

// A colour with alpha (0..1 each).
struct Rgba {
    float r = 0, g = 0, b = 0, a = 1;
};

class Overlay {
public:
    Overlay();
    ~Overlay();
    bool init(const std::vector<fs::path> &fontFiles);
    // Fonts in front of the others (a UI theme's list, in order: each character comes from the first that has it,
    // like CSS font-family), each sized to the same capital height as the first built-in font, times `scale`. The
    // built-in fonts stay as the last fallbacks. An empty list removes them. Returns how many loaded.
    int setThemeFonts(const std::vector<fs::path> &files, float scale = 1);
    void shutdown();

    void begin(int width, int height);
    // Draws UTF-8 text with its baseline at (x, y) in pixels from the top-left.
    // Returns the advance width.
    float text(const std::string &utf8, float x, float y, float sizePx, float r, float g, float b, float a,
               bool shadow = true);
    float measure(const std::string &utf8, float sizePx);
    // Where the text's ink starts and ends horizontally, relative to the x it is drawn at (the advance width that
    // measure() gives also counts the letters' side spacing).
    void inkSpan(const std::string &utf8, float sizePx, float &x0, float &x1);
    // How far capital letters reach above the baseline at this size (the height of "H").
    float capHeight(float sizePx);
    void rect(float x, float y, float w, float h, float r, float g, float b, float a);
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a);
    // A box with rounded corners (radius), antialiased: filled with a vertical gradient (top -> bottom colour), or
    // with border > 0 just its outline that wide. softness = edge blur in pixels (1 = crisp; large = a shadow).
    void box(float x, float y, float w, float h, const Rgba &top, const Rgba &bottom, float radius, float border = 0,
             float softness = 1);
    // An RGBA texture (e.g. album art) at (x, y) with size w x h, faded by `a`.
    void image(GLuint texture, float x, float y, float w, float h, float a);
    void end();

    bool hasFont() const { return !fonts_.empty(); }

private:
    struct Glyph {
        float u0, v0, u1, v1;
        float xoff, yoff, w, h, advance;
    };
    struct Font {
        Bytes data;
        std::unique_ptr<stbtt_fontinfo> info;
        float scale = 1; // (theme fonts) its size against the asked one
    };
    const Glyph *glyph(uint32_t cp, int sizePx);
    void flush();
    void vertex(float x, float y, float u, float v, float r, float g, float b, float a, const float *box = nullptr,
                const float *shape = nullptr);

    std::vector<Font> fonts_;
    int themeFonts_ = 0; // fonts_[0 .. themeFonts_) are the theme's
    std::map<uint64_t, Glyph> cache_;
    GLuint atlas_ = 0, prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint screenLoc_ = -1, imageLoc_ = -1;
    static constexpr int kAtlas = 1024;
    int penX_ = 0, penY_ = 0, rowH_ = 0;
    int width_ = 0, height_ = 0;
    static constexpr int kFloats = 16; // per vertex: x, y, u, v (u < 0 = solid), r, g, b, a, box[4], shape[4]
    std::vector<float> batch_;
};

std::string formatTime(double seconds);
