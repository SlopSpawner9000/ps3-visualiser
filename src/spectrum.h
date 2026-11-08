// Produces the per-channel 256-bin magnitude spectrum the PS3 visualizers are
// fed with, and resamples it into the 128 terrain columns the same way
// qgl_canyon_app does (left channel low->high, then right channel high->low).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

constexpr float kPi = 3.14159265358979f;

class Spectrum {
public:
    static constexpr int kSize = 2048;
    static constexpr int kBins = kSize / 2;

    // `length` < kSize analyses only the most recent `length` samples (Hann
    // window, zero-padded): faster reaction, coarser bass resolution.
    explicit Spectrum(int length = kSize)
    {
        length = std::clamp(length, 64, kSize);
        window_.assign(kSize, 0.0f);
        float sum = 0;
        for (int i = 0; i < length; i++) {
            window_[kSize - length + i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / (length - 1));
            sum += window_[kSize - length + i];
        }
        // A full-scale sine at a bin centre reads 1.0.
        norm_ = 2.0f / sum;
    }

    // `samples` holds kSize frames; writes kBins linear magnitudes.
    void analyse(const float *samples, float *bins)
    {
        std::vector<std::complex<float>> x(kSize);
        for (int i = 0; i < kSize; i++)
            x[i] = samples[i] * window_[i];
        fft(x);
        for (int k = 0; k < kBins; k++)
            bins[k] = std::abs(x[k]) * norm_;
    }

    // Column layout from qgl_canyon_app: columns 0..63 show the left channel
    // from low to high frequencies, columns 64..127 the right channel from high
    // to low, so the highs meet in the middle. The PS3 point-samples a linear
    // 256-bin spectrum, which leaves almost every column empty for real music;
    // here each column is the RMS of a log-spaced band from kLowHz to kHighHz.
    static constexpr float kLowHz = 40.0f, kHighHz = 5000.0f;   // 5 kHz: calibrated against a PS3 recording

    static void toColumns(const float *left, const float *right, float *columns, float sampleRate,
                          float lowHz = kLowHz, float highHz = kHighHz)
    {
        const float binHz = sampleRate / float(kSize);
        for (int i = 0; i < 64; i++) {
            const float f0 = lowHz * std::pow(highHz / lowHz, float(i) / 64.0f);
            const float f1 = lowHz * std::pow(highHz / lowHz, float(i + 1) / 64.0f);
            const int k0 = std::clamp(int(f0 / binHz), 1, kBins - 1);
            const int k1 = std::clamp(int(f1 / binHz), k0 + 1, kBins);
            float l = 0, r = 0;
            for (int k = k0; k < k1; k++) {
                l += left[k] * left[k];
                r += right[k] * right[k];
            }
            columns[i] = std::sqrt(l / float(k1 - k0));
            columns[127 - i] = std::sqrt(r / float(k1 - k0));
        }
    }

    // The PS3's own layout: linear frequency spacing, left channel low->high,
    // right channel high->low. Each column is the RMS of an equal-width band
    // from 0 to `highHz` (skipping DC).
    static void toColumnsLinear(const float *left, const float *right, float *columns, float sampleRate,
                                float highHz)
    {
        const float binHz = sampleRate / float(kSize);
        for (int i = 0; i < 64; i++) {
            const int k0 = std::clamp(int(float(i) / 64.0f * highHz / binHz), 1, kBins - 1);
            const int k1 = std::clamp(int(float(i + 1) / 64.0f * highHz / binHz), k0 + 1, kBins);
            float l = 0, r = 0;
            for (int k = k0; k < k1; k++) {
                l += left[k] * left[k];
                r += right[k] * right[k];
            }
            columns[i] = std::sqrt(l / float(k1 - k0));
            columns[127 - i] = std::sqrt(r / float(k1 - k0));
        }
    }

    static void fft(std::vector<std::complex<float>> &a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; i++) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * kPi / float(len);
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<float> w(1.0f);
                for (size_t j = 0; j < len / 2; j++) {
                    const auto u = a[i + j], v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

private:
    std::vector<float> window_;
    float norm_ = 1.0f;
};

// The spectrum feed of the PS3's soundvisualizer_plugin (sub_1c9c), which the
// visualizers receive as {u32; float L[256]; float R[256]}: the last 512 samples
// at 48 kHz, a Gaussian window, FFT scaled by 1/512, magnitudes smoothed across
// frequency with a 17-tap Gaussian, remapped onto a 4-octave exponential
// frequency scale and log-compressed to 3*ln(1 + 512 v) / ln(513).
class Ps3Feed {
public:
    static constexpr int kSamples = 512, kOut = 256;

    Ps3Feed()
    {
        gauss(256, window_.data()); // 513 taps, the last one unused
        kernelSum_ = gauss(8, kernel_.data());
        for (int j = 0; j < kOut; j++)
            pos_[j] = (std::pow(2.0f, (float(j) + 1.0f / 64.0f) / 64.0f) - 1.0f) * 255.0f /
                      (std::pow(2.0f, 3.98462f) - 1.0f);
    }

    // `samples`: the most recent kSamples of one channel (+-1 floats).
    void analyse(const float *samples, float scale, float *out) const
    {
        std::vector<std::complex<float>> x(kSamples);
        for (int i = 0; i < kSamples; i++)
            x[i] = samples[i] * scale * window_[i];
        Spectrum::fft(x);
        float mag[kOut], smooth[kOut] = {};
        for (int k = 0; k < kOut; k++)
            mag[k] = std::abs(x[k]) / float(kSamples);
        for (int i = 0; i < kOut; i++)
            for (int t = 0; t < 17; t++)
                if (const int k = i - 8 + t; k >= 0 && k < kOut)
                    smooth[k] += mag[i] / kernelSum_ * kernel_[t];
        for (int j = 0; j < kOut; j++) {
            const int k = int(pos_[j]);
            const float f = pos_[j] - float(k);
            const float v = f > 0 ? (1 - f) * smooth[k] + f * smooth[std::min(k + 1, kOut - 1)] : smooth[k];
            out[j] = 3.0f * std::log(512.0f * v + 1.0f) / std::log(513.0f);
        }
    }

    // qgl_canyon_app point-samples 64 entries per channel: left low->high in
    // columns 0..63, right high->low in 64..127.
    static void toColumns(const float *left, const float *right, float *columns)
    {
        for (int i = 0; i < 64; i++) {
            columns[i] = left[int(float(i + 1) * (1.0f / 64.0f) * 255.0f)];
            columns[64 + i] = right[int(float(i + 1) * (1.0f / 64.0f) * -255.0f + 255.0f)];
        }
    }

private:
    // sub_1aec: 2n+1 Gaussian taps over +-2.355, shifted so the ends are 0; returns the sum.
    static float gauss(int n, float *w)
    {
        for (int k = 0; k <= 2 * n; k++) {
            const float t = float(k - n) * 2.355f / float(n);
            w[k] = 2.5f * std::pow(2.7f, -t * t / 0.56f) / std::sqrt(6.36159f);
        }
        float sum = 0;
        const float edge = w[0];
        for (int k = 0; k <= 2 * n; k++)
            sum += (w[k] -= edge);
        return sum;
    }

    std::array<float, 513> window_{};
    std::array<float, 17> kernel_{};
    float kernelSum_ = 1;
    std::array<float, kOut> pos_{};
};
