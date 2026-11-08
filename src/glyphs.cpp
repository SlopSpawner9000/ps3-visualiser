#include "glyphs.h"
#include "overlay.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDark[3] = {0.13f, 0.13f, 0.16f}, kEdge[3] = {0.55f, 0.56f, 0.6f};

void disc(Overlay &o, float cx, float cy, float r, const float *c, float a)
{
    constexpr int n = 28;
    for (int i = 0; i < n; i++) {
        const float a0 = 2 * kPi * float(i) / n, a1 = 2 * kPi * float(i + 1) / n;
        o.tri(cx, cy, cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), c[0], c[1],
              c[2], a);
    }
}

void ring(Overlay &o, float cx, float cy, float r, float thick, const float *c, float a)
{
    constexpr int n = 28;
    const float ri = r - thick;
    for (int i = 0; i < n; i++) {
        const float a0 = 2 * kPi * float(i) / n, a1 = 2 * kPi * float(i + 1) / n;
        const float x0 = std::cos(a0), y0 = std::sin(a0), x1 = std::cos(a1), y1 = std::sin(a1);
        o.tri(cx + r * x0, cy + r * y0, cx + r * x1, cy + r * y1, cx + ri * x0, cy + ri * y0, c[0], c[1], c[2], a);
        o.tri(cx + r * x1, cy + r * y1, cx + ri * x1, cy + ri * y1, cx + ri * x0, cy + ri * y0, c[0], c[1], c[2], a);
    }
}

void line(Overlay &o, float x0, float y0, float x1, float y1, float thick, const float *c, float a)
{
    const float dx = x1 - x0, dy = y1 - y0, len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
    const float nx = -dy / len * thick * 0.5f, ny = dx / len * thick * 0.5f;
    o.tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c[0], c[1], c[2], a);
    o.tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c[0], c[1], c[2], a);
}

void centredText(Overlay &o, const std::string &t, float cx, float cy, float size, const float *c, float a)
{
    o.text(t, cx - o.measure(t, size) * 0.5f, cy + size * 0.36f, size, c[0], c[1], c[2], a, false);
}

bool face(PadButton b) { return b >= PadButton::South && b <= PadButton::North; }
bool pill(PadButton b, PadStyle s)
{
    return b == PadButton::LeftShoulder || b == PadButton::RightShoulder || b == PadButton::LeftTrigger ||
           b == PadButton::RightTrigger || b == PadButton::Guide ||
           (s == PadStyle::PlayStation && (b == PadButton::Start || b == PadButton::Back));
}

} // namespace

std::string padButtonName(PadButton b, PadStyle s)
{
    const bool ps = s == PadStyle::PlayStation, xbox = s == PadStyle::Xbox;
    switch (b) {
    case PadButton::South: return ps ? "Cross" : "A";
    case PadButton::East: return ps ? "Circle" : "B";
    case PadButton::West: return ps ? "Square" : "X";
    case PadButton::North: return ps ? "Triangle" : "Y";
    case PadButton::Back: return ps ? "Create" : "View";
    case PadButton::Guide: return ps ? "PS" : xbox ? "Xbox" : "Steam";
    case PadButton::Start: return ps ? "Options" : "Menu";
    case PadButton::LeftStick: return xbox ? "LS" : "L3";
    case PadButton::RightStick: return xbox ? "RS" : "R3";
    case PadButton::LeftShoulder: return xbox ? "LB" : "L1";
    case PadButton::RightShoulder: return xbox ? "RB" : "R1";
    case PadButton::LeftTrigger: return xbox ? "LT" : "L2";
    case PadButton::RightTrigger: return xbox ? "RT" : "R2";
    case PadButton::DpadUp: return "D-pad up";
    case PadButton::DpadDown: return "D-pad down";
    case PadButton::DpadLeft: return "D-pad left";
    case PadButton::DpadRight: return "D-pad right";
    default: return "-";
    }
}

float padGlyphWidth(Overlay &o, PadButton b, PadStyle s, float size)
{
    if (b == PadButton::None)
        return o.measure("-", size * 0.8f);
    if (pill(b, s))
        return std::max(size * 1.45f, o.measure(padButtonName(b, s), size * 0.5f) + size * 0.6f);
    return size;
}

float drawPadGlyph(Overlay &o, PadButton b, PadStyle s, float x, float cy, float size, float a, const Rgba *dash)
{
    const float w = padGlyphWidth(o, b, s, size), r = size * 0.5f, cx = x + w * 0.5f;
    static const float white[3] = {0.95f, 0.95f, 0.97f};
    if (b == PadButton::None) {
        const Rgba c = dash ? *dash : Rgba{1, 1, 1, 0.4f};
        o.text("-", x, cy + size * 0.3f, size * 0.8f, c.r, c.g, c.b, a * c.a);
        return w;
    }
    if (face(b)) {
        disc(o, cx, cy, r, kDark, a);
        if (s == PadStyle::PlayStation) {
            // Cross blue, Circle red, Square pink, Triangle green: outlined symbols.
            static const float cols[4][3] = {{0.55f, 0.7f, 1.0f}, {1.0f, 0.45f, 0.45f}, {0.95f, 0.55f, 0.82f}, {0.4f, 0.88f, 0.72f}};
            const float *c = cols[int(b)], t = size * 0.09f, k = r * 0.48f;
            ring(o, cx, cy, r, size * 0.05f, kEdge, a);
            if (b == PadButton::South) {
                line(o, cx - k, cy - k, cx + k, cy + k, t, c, a);
                line(o, cx - k, cy + k, cx + k, cy - k, t, c, a);
            } else if (b == PadButton::East) {
                ring(o, cx, cy, k * 1.05f, t, c, a);
            } else if (b == PadButton::West) {
                const float q = k * 0.85f;
                line(o, cx - q, cy - q, cx + q, cy - q, t, c, a);
                line(o, cx + q, cy - q, cx + q, cy + q, t, c, a);
                line(o, cx + q, cy + q, cx - q, cy + q, t, c, a);
                line(o, cx - q, cy + q, cx - q, cy - q, t, c, a);
            } else {
                const float top = cy - k * 1.05f, bottom = cy + k * 0.75f, half = k * 1.05f;
                line(o, cx, top, cx + half, bottom, t, c, a);
                line(o, cx + half, bottom, cx - half, bottom, t, c, a);
                line(o, cx - half, bottom, cx, top, t, c, a);
            }
        } else {
            // Xbox: coloured letters (A green, B red, X blue, Y yellow); Steam: white letters.
            static const float cols[4][3] = {{0.45f, 0.88f, 0.4f}, {1.0f, 0.4f, 0.4f}, {0.4f, 0.65f, 1.0f}, {1.0f, 0.85f, 0.3f}};
            const float *c = s == PadStyle::Steam ? white : cols[int(b)];
            ring(o, cx, cy, r, size * 0.07f, s == PadStyle::Steam ? kEdge : c, a);
            centredText(o, padButtonName(b, s), cx, cy, size * 0.62f, c, a);
        }
        return w;
    }
    if (b >= PadButton::DpadUp && b <= PadButton::DpadRight) {
        // A plus, the pressed arm lit.
        const float arm = size * 0.36f, half = r, e = size * 0.06f;
        o.rect(cx - arm * 0.5f - e, cy - half - e, arm + 2 * e, size + 2 * e, kEdge[0], kEdge[1], kEdge[2], a);
        o.rect(cx - half - e, cy - arm * 0.5f - e, size + 2 * e, arm + 2 * e, kEdge[0], kEdge[1], kEdge[2], a);
        o.rect(cx - arm * 0.5f, cy - half, arm, size, kDark[0], kDark[1], kDark[2], a);
        o.rect(cx - half, cy - arm * 0.5f, size, arm, kDark[0], kDark[1], kDark[2], a);
        const float len = half - arm * 0.5f - size * 0.04f;
        if (b == PadButton::DpadUp)
            o.rect(cx - arm * 0.5f, cy - half, arm, len, white[0], white[1], white[2], a);
        else if (b == PadButton::DpadDown)
            o.rect(cx - arm * 0.5f, cy + half - len, arm, len, white[0], white[1], white[2], a);
        else if (b == PadButton::DpadLeft)
            o.rect(cx - half, cy - arm * 0.5f, len, arm, white[0], white[1], white[2], a);
        else
            o.rect(cx + half - len, cy - arm * 0.5f, len, arm, white[0], white[1], white[2], a);
        return w;
    }
    if (pill(b, s)) {
        // Shoulders, triggers, Guide and PlayStation's Create/Options: a labelled pill. Triggers
        // are a bit taller at the top, like the real ones.
        const bool trigger = b == PadButton::LeftTrigger || b == PadButton::RightTrigger;
        const float h = size * (trigger ? 0.86f : 0.7f), top = cy - h * 0.5f, rr = h * 0.5f;
        disc(o, x + rr, cy, rr, kEdge, a);
        disc(o, x + w - rr, cy, rr, kEdge, a);
        o.rect(x + rr, top, w - 2 * rr, h, kEdge[0], kEdge[1], kEdge[2], a);
        const float in = size * 0.06f;
        disc(o, x + rr, cy, rr - in, kDark, a);
        disc(o, x + w - rr, cy, rr - in, kDark, a);
        o.rect(x + rr, top + in, w - 2 * rr, h - 2 * in, kDark[0], kDark[1], kDark[2], a);
        centredText(o, padButtonName(b, s), cx, cy, size * 0.42f, white, a);
        return w;
    }
    // Sticks, and Menu / View on Xbox and Steam.
    disc(o, cx, cy, r, kDark, a);
    ring(o, cx, cy, r, size * 0.06f, kEdge, a);
    if (b == PadButton::Start) { // three lines
        for (int i = -1; i <= 1; i++)
            o.rect(cx - r * 0.45f, cy + float(i) * r * 0.32f - size * 0.035f, r * 0.9f, size * 0.07f, white[0], white[1],
                   white[2], a);
    } else if (b == PadButton::Back) { // two overlapping windows
        const float q = r * 0.42f, t = size * 0.06f;
        for (int k = 0; k < 2; k++) {
            const float ox = cx + (k ? r * 0.12f : -r * 0.12f), oy = cy + (k ? r * 0.12f : -r * 0.12f);
            line(o, ox - q, oy - q, ox + q, oy - q, t, white, a);
            line(o, ox + q, oy - q, ox + q, oy + q, t, white, a);
            line(o, ox + q, oy + q, ox - q, oy + q, t, white, a);
            line(o, ox - q, oy + q, ox - q, oy - q, t, white, a);
        }
    } else {
        centredText(o, padButtonName(b, s), cx, cy, size * 0.42f, white, a);
    }
    return w;
}
