// Colour palette from a track's embedded album art (Visualizer tab: "Colours from album art").
#pragma once

#include <filesystem>
#include <vector>

struct AlbumPalette {
    float hue = 0;    // main hue, degrees (0..360)
    float accent = 0; // second hue, for the line; equal to `hue` for one-colour covers
    float sat = 0;    // typical saturation of the cover's colourful pixels (0..1)
};

// False when the file has no embedded picture, it can't be decoded, or it is (almost) grey.
bool albumPalette(const std::filesystem::path &track, AlbumPalette &out);

// The embedded picture decoded to RGBA (top row first). False when there is none.
bool albumImage(const std::filesystem::path &track, std::vector<unsigned char> &rgba, int &width, int &height);
