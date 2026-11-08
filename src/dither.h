// Final copy of a visualizer's image to the screen with dithering: a little noise, under one output step, that
// breaks smooth gradients' colour bands up (Misc > Dithering).
#pragma once

#include "gl.h"

namespace dither {

// amount: 0 off (the caller blits instead), else the noise's size in output steps (1/255; 1/1023 for HDR).
// linear: the image is linear light for the HDR presenter (dithered in a gamma-like encoding there).
void copy(GLuint srcTex, int srcW, int srcH, GLuint targetFbo, int dstW, int dstH, float amount, bool linear);
void shutdown();

// The setting's levels (Off, Low, Medium, High) as amounts.
inline float amountFor(int level)
{
    static const float amounts[] = {0.0f, 0.5f, 1.0f, 2.0f};
    return amounts[level < 0 ? 0 : level > 3 ? 3 : level];
}

} // namespace dither
