// A basic 10-band graphic equalizer (octave bands, peaking biquads) with a preamp.
// Used on the audio output and, optionally, on the audio the visualizer analyses.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

struct EqSettings {
    static constexpr int kBands = 10;
    static constexpr float kFreqs[kBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    static constexpr float kMaxDb = 12.0f;

    bool enabled = false;
    enum Target { AudioOnly, AudioAndVisualizer, VisualizerOnly };
    int target = AudioOnly;

    bool affectsAudio() const { return enabled && target != VisualizerOnly; }
    bool affectsVisualizer() const { return enabled && target != AudioOnly; }
    float preampDb = 0.0f;
    std::array<float, kBands> gainDb{};

    bool operator==(const EqSettings &) const = default;
};

class Equalizer {
public:
    void configure(const EqSettings &s, float rate)
    {
        preamp_ = std::pow(10.0f, s.preampDb / 20.0f);
        for (int b = 0; b < EqSettings::kBands; b++) {
            Band &band = bands_[size_t(b)];
            band.active = std::fabs(s.gainDb[size_t(b)]) > 0.01f && EqSettings::kFreqs[b] < rate * 0.45f;
            if (!band.active)
                continue;
            // RBJ cookbook peaking filter, one octave wide.
            const float a = std::pow(10.0f, s.gainDb[size_t(b)] / 40.0f);
            const float w0 = 2.0f * 3.14159265f * EqSettings::kFreqs[b] / rate;
            const float alpha = std::sin(w0) / (2.0f * 1.41f), cw = std::cos(w0);
            const float a0 = 1.0f + alpha / a;
            band.b0 = (1.0f + alpha * a) / a0;
            band.b1 = -2.0f * cw / a0;
            band.b2 = (1.0f - alpha * a) / a0;
            band.a1 = -2.0f * cw / a0;
            band.a2 = (1.0f - alpha / a) / a0;
        }
    }

    // Gain in dB (preamp included) at frequency `hz`, for display.
    float responseDb(float hz, float rate) const
    {
        const float w = 2.0f * 3.14159265f * hz / rate;
        const float c1 = std::cos(w), c2 = std::cos(2 * w), s1 = std::sin(w), s2 = std::sin(2 * w);
        float db = 20.0f * std::log10(preamp_);
        for (const Band &b : bands_) {
            if (!b.active)
                continue;
            const float nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = -(b.b1 * s1 + b.b2 * s2);
            const float dr = 1.0f + b.a1 * c1 + b.a2 * c2, di = -(b.a1 * s1 + b.a2 * s2);
            db += 10.0f * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
        }
        return db;
    }

    // Filters `n` frames of separate left/right channels in place; keeps filter state
    // between calls.
    void process(float *left, float *right, int n)
    {
        float *ch[2] = {left, right};
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < n; i++) {
                float x = ch[c][i] * preamp_;
                for (Band &b : bands_) {
                    if (!b.active)
                        continue;
                    float *z = b.z[c];
                    const float y = b.b0 * x + z[0];
                    z[0] = b.b1 * x - b.a1 * y + z[1];
                    z[1] = b.b2 * x - b.a2 * y;
                    x = y;
                }
                ch[c][i] = x;
            }
    }

private:
    struct Band {
        bool active = false;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};
    };
    std::array<Band, EqSettings::kBands> bands_{};
    float preamp_ = 1.0f;
};
