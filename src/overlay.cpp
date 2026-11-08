#include "overlay.h"

#include "cg.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const char *kVs = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 col;
layout(location = 3) in vec4 box;   // shapes: position relative to the centre, half size
layout(location = 4) in vec4 shape; // shapes: corner radius, border width (0 = filled), edge softness, kind (1 = shape)
uniform vec2 screen;
out vec2 vUv;
out vec4 vCol;
out vec4 vBox;
out vec4 vShape;
void main() {
    vUv = uv;
    vCol = col;
    vBox = box;
    vShape = shape;
    gl_Position = vec4(pos / screen * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

const char *kFs = R"(#version 330 core
uniform sampler2D atlas;
uniform int image; // 1: draw an RGBA picture (album art) instead of text/shapes
in vec2 vUv;
in vec4 vCol;
in vec4 vBox;
in vec4 vShape;
out vec4 fragColor;
void main() {
    if (vShape.w > 0.5) { // rounded box: signed distance to its edge, antialiased; or a ring (border), or a shadow
        vec2 q = abs(vBox.xy) - vBox.zw + vShape.x;
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - vShape.x;
        float soft = max(vShape.z, 1.0);
        float a = clamp(0.5 - d / soft, 0.0, 1.0);
        if (vShape.y > 0.0)
            a *= clamp(0.5 + (d + vShape.y) / soft, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
        return;
    }
    if (image == 1) {
        fragColor = texture(atlas, vUv) * vCol;
        return;
    }
    float a = vUv.x < 0.0 ? 1.0 : texture(atlas, vUv).r;
    fragColor = vec4(vCol.rgb, vCol.a * a);
}
)";

uint32_t nextCodepoint(const std::string &s, size_t &i)
{
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
        return c;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    uint32_t cp = c & (0x3f >> extra);
    while (extra-- > 0 && i < s.size())
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3f);
    return cp;
}

} // namespace

std::string formatTime(double seconds)
{
    if (!(seconds >= 0))
        seconds = 0;
    const int t = int(seconds);
    char buf[32];
    if (t >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

Overlay::Overlay() = default;
Overlay::~Overlay() = default;

bool Overlay::init(const std::vector<fs::path> &fontFiles)
{
    for (auto &f : fontFiles) {
        auto data = readFile(f);
        if (!data)
            continue;
        Font font;
        font.data = std::move(*data);
        font.info = std::make_unique<stbtt_fontinfo>();
        if (stbtt_InitFont(font.info.get(), font.data.data(), stbtt_GetFontOffsetForIndex(font.data.data(), 0)))
            fonts_.push_back(std::move(font));
    }
    prog_ = compileProgram(kVs, kFs, "overlay");
    if (!prog_)
        return false;
    screenLoc_ = glGetUniformLocation(prog_, "screen");
    imageLoc_ = glGetUniformLocation(prog_, "image");
    glGenTextures(1, &atlas_);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::vector<uint8_t> zero(size_t(kAtlas) * kAtlas, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlas, kAtlas, 0, GL_RED, GL_UNSIGNED_BYTE, zero.data());
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    constexpr GLsizei stride = kFloats * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(8));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(16));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(32));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(48));
    glBindVertexArray(0);
    return true;
}

void Overlay::shutdown()
{
    glDeleteTextures(1, &atlas_);
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteProgram(prog_);
}

int Overlay::setThemeFonts(const std::vector<fs::path> &files, float scale)
{
    fonts_.erase(fonts_.begin(), fonts_.begin() + themeFonts_);
    themeFonts_ = 0;
    // Same capital height as the first built-in font (fonts differ a lot in how much of their "pixel height" the
    // letters fill), then the theme's own factor.
    auto capHeight = [](const stbtt_fontinfo *f) {
        int x0, y0, x1, y1;
        return stbtt_GetCodepointBox(f, 'H', &x0, &y0, &x1, &y1) ? float(y1 - y0) * stbtt_ScaleForPixelHeight(f, 1.0f)
                                                                 : 0.0f;
    };
    const float ref = fonts_.empty() ? 0 : capHeight(fonts_.front().info.get());
    for (const fs::path &file : files) {
        auto data = readFile(file);
        if (!data)
            continue;
        Font font;
        font.data = std::move(*data);
        font.info = std::make_unique<stbtt_fontinfo>();
        const int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
        if (offset < 0 || !stbtt_InitFont(font.info.get(), font.data.data(), offset))
            continue;
        const float mine = capHeight(font.info.get());
        font.scale = std::clamp((mine > 0 && ref > 0 ? ref / mine : 1.0f) * scale, 0.5f, 2.5f);
        fonts_.insert(fonts_.begin() + themeFonts_, std::move(font));
        themeFonts_++;
    }
    cache_.clear(); // (the atlas is refilled from the top)
    penX_ = penY_ = rowH_ = 0;
    return themeFonts_;
}

const Overlay::Glyph *Overlay::glyph(uint32_t cp, int sizePx)
{
    const uint64_t key = (uint64_t(sizePx) << 32) | cp;
    auto it = cache_.find(key);
    if (it != cache_.end())
        return &it->second;
    const Font *font = nullptr;
    int index = 0;
    for (auto &f : fonts_) {
        index = stbtt_FindGlyphIndex(f.info.get(), int(cp));
        if (index) {
            font = &f;
            break;
        }
    }
    if (!font) {
        if (cp == '?' || fonts_.empty())
            return nullptr;
        return glyph('?', sizePx);
    }
    const float scale = stbtt_ScaleForPixelHeight(font->info.get(), float(sizePx) * font->scale);
    int adv = 0, lsb = 0, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(font->info.get(), index, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(font->info.get(), index, scale, scale, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0, gh = y1 - y0;
    if (penX_ + gw + 1 >= kAtlas) {
        penX_ = 0;
        penY_ += rowH_ + 1;
        rowH_ = 0;
    }
    if (penY_ + gh + 1 >= kAtlas) {
        // Atlas full: start over (glyphs are re-rasterised on demand).
        cache_.clear();
        penX_ = penY_ = rowH_ = 0;
        std::vector<uint8_t> zero(size_t(kAtlas) * kAtlas, 0);
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlas, kAtlas, GL_RED, GL_UNSIGNED_BYTE, zero.data());
    }
    Glyph g{};
    if (gw > 0 && gh > 0) {
        std::vector<uint8_t> bmp(size_t(gw) * gh);
        stbtt_MakeGlyphBitmap(font->info.get(), bmp.data(), gw, gh, gw, scale, scale, index);
        glBindTexture(GL_TEXTURE_2D, atlas_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, penX_, penY_, gw, gh, GL_RED, GL_UNSIGNED_BYTE, bmp.data());
        g.u0 = float(penX_) / kAtlas;
        g.v0 = float(penY_) / kAtlas;
        g.u1 = float(penX_ + gw) / kAtlas;
        g.v1 = float(penY_ + gh) / kAtlas;
        penX_ += gw + 1;
        rowH_ = std::max(rowH_, gh);
    }
    g.xoff = float(x0);
    g.yoff = float(y0);
    g.w = float(gw);
    g.h = float(gh);
    g.advance = float(adv) * scale;
    return &cache_.emplace(key, g).first->second;
}

void Overlay::begin(int width, int height)
{
    width_ = width;
    height_ = height;
    batch_.clear();
}

void Overlay::rect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    // Hairlines (thinner than ~1.5 px, e.g. 1 unit at a small UI scale) snap to whole pixels, at
    // least 1 px thick: unsnapped they can fall between pixel centres and vanish.
    auto snap = [](float &pos, float &size) {
        if (size > 0 && size < 1.5f) {
            pos = std::floor(pos + size * 0.5f);
            size = 1.0f;
        }
    };
    snap(y, h);
    snap(x, w);
    const float v[6][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y}, {x + w, y + h}, {x, y + h}};
    for (auto &p : v)
        vertex(p[0], p[1], -1.0f, -1.0f, r, g, b, a);
}

void Overlay::vertex(float x, float y, float u, float v, float r, float g, float b, float a, const float *box, const float *shape)
{
    static const float none[4] = {0, 0, 0, 0};
    box = box ? box : none;
    shape = shape ? shape : none;
    batch_.insert(batch_.end(), {x, y, u, v, r, g, b, a, box[0], box[1], box[2], box[3], shape[0], shape[1], shape[2], shape[3]});
}

void Overlay::box(float x, float y, float w, float h, const Rgba &top, const Rgba &bottom, float radius, float border,
                  float softness)
{
    if (w <= 0 || h <= 0 || (top.a <= 0 && bottom.a <= 0))
        return;
    // The quad reaches past the box by the softness (shadows blur outwards); the shader cuts the shape.
    const float grow = std::max(softness, 1.0f), cx = x + w * 0.5f, cy = y + h * 0.5f;
    radius = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
    const float shape[4] = {radius, border, softness, 1.0f};
    const float x0 = x - grow, y0 = y - grow, x1 = x + w + grow, y1 = y + h + grow;
    auto at = [&](float px, float py) {
        const float t = std::clamp((py - y) / h, 0.0f, 1.0f); // vertical gradient
        const float b[4] = {px - cx, py - cy, w * 0.5f, h * 0.5f};
        vertex(px, py, -1.0f, -1.0f, top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
               top.b + (bottom.b - top.b) * t, top.a + (bottom.a - top.a) * t, b, shape);
    };
    // (Two rows of quads so the gradient runs exactly from the box's top to its bottom, also when the quad grows.)
    const float ys[4] = {y0, y, y + h, y1};
    for (int k = 0; k < 3; k++) {
        if (ys[k + 1] <= ys[k])
            continue;
        at(x0, ys[k]), at(x1, ys[k]), at(x1, ys[k + 1]);
        at(x0, ys[k]), at(x1, ys[k + 1]), at(x0, ys[k + 1]);
    }
}

void Overlay::tri(float x0, float y0, float x1, float y1, float x2, float y2, float r, float g, float b, float a)
{
    const float v[3][2] = {{x0, y0}, {x1, y1}, {x2, y2}};
    for (auto &q : v)
        vertex(q[0], q[1], -1.0f, -1.0f, r, g, b, a);
}

float Overlay::measure(const std::string &utf8, float sizePx)
{
    float w = 0;
    const int px = std::max(4, int(std::lround(sizePx)));
    for (size_t i = 0; i < utf8.size();)
        if (const Glyph *g = glyph(nextCodepoint(utf8, i), px))
            w += g->advance;
    return w;
}

void Overlay::inkSpan(const std::string &utf8, float sizePx, float &x0, float &x1)
{
    const int px = std::max(4, int(std::lround(sizePx)));
    float x = 0;
    x0 = 1e9f, x1 = -1e9f;
    for (size_t i = 0; i < utf8.size();)
        if (const Glyph *g = glyph(nextCodepoint(utf8, i), px)) {
            if (g->w > 0) {
                x0 = std::min(x0, x + g->xoff);
                x1 = std::max(x1, x + g->xoff + g->w);
            }
            x += g->advance;
        }
    if (x0 > x1)
        x0 = 0, x1 = x;
}

float Overlay::capHeight(float sizePx)
{
    const Glyph *g = glyph('H', std::max(4, int(std::lround(sizePx))));
    return g && g->h > 0 ? -g->yoff : sizePx * 0.7f;
}

float Overlay::text(const std::string &utf8, float x, float y, float sizePx, float r, float g, float b, float a,
                    bool shadow)
{
    const int px = std::max(4, int(std::lround(sizePx)));
    if (shadow) {
        const float o = std::max(1.0f, sizePx / 18.0f);
        text(utf8, x + o, y + o, sizePx, 0, 0, 0, a * 0.6f, false);
    }
    const float start = x;
    for (size_t i = 0; i < utf8.size();) {
        const Glyph *gl = glyph(nextCodepoint(utf8, i), px);
        if (!gl)
            continue;
        if (gl->w > 0) {
            const float x0 = std::round(x + gl->xoff), y0 = std::round(y + gl->yoff);
            const float x1 = x0 + gl->w, y1 = y0 + gl->h;
            const float v[6][4] = {{x0, y0, gl->u0, gl->v0}, {x1, y0, gl->u1, gl->v0}, {x1, y1, gl->u1, gl->v1},
                                   {x0, y0, gl->u0, gl->v0}, {x1, y1, gl->u1, gl->v1}, {x0, y1, gl->u0, gl->v1}};
            for (auto &q : v)
                vertex(q[0], q[1], q[2], q[3], r, g, b, a);
        }
        x += gl->advance;
    }
    return x - start;
}

void Overlay::end() { flush(); }

void Overlay::image(GLuint texture, float x, float y, float w, float h, float a)
{
    flush(); // keep the drawing order: everything queued so far goes first
    const float v[6][4] = {{x, y, 0, 0}, {x + w, y, 1, 0}, {x + w, y + h, 1, 1},
                           {x, y, 0, 0}, {x + w, y + h, 1, 1}, {x, y + h, 0, 1}};
    for (auto &p : v)
        vertex(p[0], p[1], p[2], p[3], 1.0f, 1.0f, 1.0f, a);
    glUseProgram(prog_);
    glUniform1i(imageLoc_, 1);
    const GLuint atlas = atlas_;
    atlas_ = texture;
    flush();
    atlas_ = atlas;
    glUseProgram(prog_);
    glUniform1i(imageLoc_, 0);
}

void Overlay::flush()
{
    if (batch_.empty())
        return;
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform2f(screenLoc_, float(width_), float(height_));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batch_.size() * sizeof(float)), batch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch_.size() / kFloats));
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    batch_.clear();
}
