// Controller button glyphs drawn from simple shapes, in Xbox, PlayStation and Steam styles.
#pragma once

#include "gamepad.h"

#include <string>

class Overlay;
struct Rgba;

// Width of a glyph `size` pixels tall (most are square; pills for shoulders and Start/Back are wider).
float padGlyphWidth(Overlay &o, PadButton b, PadStyle style, float size);
// Draws it with its left edge at x, centred on cy. Returns the width. `dash` colours the "-" of an unbound
// button (default faint white; pass the text colour on light backgrounds).
float drawPadGlyph(Overlay &o, PadButton b, PadStyle style, float x, float cy, float size, float alpha,
                   const Rgba *dash = nullptr);
// The button's printed name, e.g. "A" / "Cross", "LB" / "L1", "Menu" / "Options".
std::string padButtonName(PadButton b, PadStyle style);
