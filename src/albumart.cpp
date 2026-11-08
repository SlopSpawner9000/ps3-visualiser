#include "albumart.h"
#include "audio.h"

#include <SDL3/SDL.h>

#include "stb_image.h" // implementation lives in firmware.cpp (PNG + JPEG)

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// h in degrees, s and v 0..1.
void rgbToHsv(float r, float g, float b, float &h, float &s, float &v)
{
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        h = 0;
        return;
    }
    if (mx == r)
        h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g)
        h = 60.0f * ((b - r) / d + 2.0f);
    else
        h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0)
        h += 360.0f;
}

float hueDistance(float a, float b)
{
    const float d = std::fabs(a - b);
    return std::min(d, 360.0f - d);
}

} // namespace

bool albumPalette(const std::filesystem::path &track, AlbumPalette &out)
{
    std::vector<uint8_t> encoded;
    if (!AudioEngine::coverArt(track, encoded))
        return false;
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(encoded.data(), int(encoded.size()), &w, &h, &n, 3);
    if (!px) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "album art: can't decode the picture (%s)", stbi_failure_reason());
        return false;
    }
    // Hue histogram (10-degree bins) of a 64x64 sample grid, weighted by how colourful each
    // pixel is; greys, near-black and near-white pixels barely count.
    constexpr int kBins = 36, kGrid = 64;
    float bins[kBins] = {}, satSum = 0, weightSum = 0;
    for (int gy = 0; gy < kGrid; gy++)
        for (int gx = 0; gx < kGrid; gx++) {
            const unsigned char *p = px + (size_t(gy * h / kGrid) * size_t(w) + size_t(gx * w / kGrid)) * 3;
            float hue, s, v;
            rgbToHsv(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, hue, s, v);
            const float weight = s * s * std::clamp((v - 0.12f) * 3.0f, 0.0f, 1.0f);
            bins[int(hue / 10.0f) % kBins] += weight;
            satSum += s * weight;
            weightSum += weight;
        }
    stbi_image_free(px);
    if (weightSum < 0.02f * kGrid * kGrid) // (almost) no colour in the picture
        return false;
    float smooth[kBins];
    for (int i = 0; i < kBins; i++)
        smooth[i] = bins[(i + kBins - 1) % kBins] * 0.25f + bins[i] * 0.5f + bins[(i + 1) % kBins] * 0.25f;
    const int peak = int(std::max_element(smooth, smooth + kBins) - smooth);
    // Weighted circular mean around the peak for a precise main hue.
    auto meanAround = [&](int centre) {
        float x = 0, y = 0;
        for (int k = -2; k <= 2; k++) {
            const int i = (centre + k + kBins) % kBins;
            const float a = (float(i) + 0.5f) * 10.0f * 3.14159265f / 180.0f;
            x += std::cos(a) * bins[i];
            y += std::sin(a) * bins[i];
        }
        float deg = std::atan2(y, x) * 180.0f / 3.14159265f;
        return deg < 0 ? deg + 360.0f : deg;
    };
    out.hue = meanAround(peak);
    out.accent = out.hue;
    int second = -1;
    for (int i = 0; i < kBins; i++)
        if (hueDistance((float(i) + 0.5f) * 10.0f, out.hue) >= 60.0f && smooth[i] >= 0.15f * smooth[peak] &&
            (second < 0 || smooth[i] > smooth[second]))
            second = i;
    if (second >= 0)
        out.accent = meanAround(second);
    out.sat = satSum / weightSum;
    SDL_Log("album art %dx%d: hue %.0f, accent %.0f, saturation %.2f", w, h, double(out.hue), double(out.accent),
            double(out.sat));
    return true;
}

bool albumImage(const std::filesystem::path &track, std::vector<unsigned char> &rgba, int &width, int &height)
{
    std::vector<uint8_t> encoded;
    if (!AudioEngine::coverArt(track, encoded))
        return false;
    int n = 0;
    unsigned char *px = stbi_load_from_memory(encoded.data(), int(encoded.size()), &width, &height, &n, 4);
    if (!px)
        return false;
    rgba.assign(px, px + size_t(width) * size_t(height) * 4);
    stbi_image_free(px);
    return true;
}
