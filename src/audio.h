// Audio playback: FFmpeg decodes on a worker thread into a ring buffer that an
// SDL3 audio stream drains. Everything that reaches the device is also copied
// into a small history buffer that the visualizer analyses.
#pragma once

#include "eq.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SDL_AudioStream;

struct TrackInfo {
    std::string title, artist, album;
    double duration = 0;
    int sampleRate = 0; // of the file, before resampling to kRate
};

class AudioEngine {
public:
    static constexpr int kRate = 48000;
    static constexpr int kHistory = 8192;

    bool init();
    void shutdown();

    // `fadeIn` false: start at full volume even when song fades are on.
    bool open(const std::filesystem::path &path, bool fadeIn = true);
    void stop();
    bool isOpen() const { return voices_[mix_ & 1].open; }

    void setPaused(bool p);
    bool paused() const { return paused_; }
    void seek(double seconds);
    double position() const;
    double duration() const { return voices_[mix_ & 1].info.duration; }
    bool finished() const;
    const TrackInfo &info() const { return voices_[mix_ & 1].info; }

    void setVolume(float v) { volume_ = v; }
    // Song fades: tracks opened with open() fade in over this long (0 = off).
    void setFade(float seconds) { fade_ = seconds; }
    // Crossfade: the current track fades out while `path` fades in (equal power) over `seconds`.
    // The new track becomes the current one immediately (info, position, seek). False if it
    // can't be opened (nothing changes then).
    bool crossfadeTo(const std::filesystem::path &path, float seconds);
    bool crossfading() const { return mix_ & 2; }
    // 0..1 through the current crossfade (1 when not crossfading).
    float crossfadeProgress() const
    {
        return crossfading() ? float(std::clamp(double(xfadePos_) / double(xfadeLen_), 0.0, 1.0)) : 1.0f;
    }
    // Seconds of the current track left to play (exact near the end, from the duration before).
    double remaining() const;
    // Call regularly from the main thread: retires the faded-out track after a crossfade.
    void update();
    // Equalizer for what you hear (the visualizer history stays unequalized).
    void setEq(const EqSettings &eq);
    float volume() const { return volume_; }

    // Copies the most recent n (<= kHistory) frames sent to the device.
    void recentSamples(float *left, float *right, int n) const;
    // Like recentSamples, but ending `ahead` frames after the last frame sent to the
    // device (taken from the decoded queue; zeros past what's decoded).
    void upcomingSamples(float *left, float *right, int n, int ahead) const;

    // Decode a whole file without playing it (used for offline rendering).
    static bool decodeFile(const std::filesystem::path &path, std::vector<float> &interleaved, TrackInfo *info);
    // The picture embedded in a file (ID3 APIC, MP4 covr, FLAC picture, ...), encoded (JPEG/PNG).
    static bool coverArt(const std::filesystem::path &path, std::vector<uint8_t> &encoded);

private:
    static void callback(void *user, SDL_AudioStream *stream, int additional, int total);

    // One decoding pipeline: a decoder thread filling a ring buffer the callback drains.
    // Two of them make crossfades possible.
    struct Voice {
        std::thread thread;
        mutable std::mutex mtx;
        std::condition_variable cv;
        std::vector<float> ring;
        size_t ringRead = 0, ringCount = 0;
        std::atomic<bool> quit{false}, eof{false}, open{false};
        std::atomic<double> seekTo{-1.0}, base{0.0};
        std::atomic<int64_t> played{0}, sinceOpen{0};
        TrackInfo info;
    };
    bool startVoice(Voice &v, const std::filesystem::path &path);
    void stopVoice(Voice &v);
    void decodeLoop(Voice *v, std::string path);
    void pushFrames(Voice &v, const float *data, int frames);
    static int pull(Voice &v, float *dst, int frames); // audio thread

    SDL_AudioStream *stream_ = nullptr;
    Voice voices_[2];
    // bit 0: index of the current voice; bit 1: crossfading from the other voice.
    std::atomic<int> mix_{0};
    std::atomic<int64_t> xfadeLen_{1}, xfadePos_{0};
    std::atomic<bool> xfadeDone_{false};
    std::atomic<bool> paused_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> fade_{0.0f};

    std::vector<float> histL_, histR_;
    std::mutex eqMtx_;
    EqSettings eqPending_;
    std::atomic<int> eqVersion_{0};
    int eqApplied_ = -1;   // audio thread only
    EqSettings eqActive_;  // audio thread only
    Equalizer eq_;         // audio thread only
    std::atomic<uint64_t> histWrite_{0};
};
