#pragma once

#include <filesystem>
#include <random>
#include <vector>

class Playlist {
public:
    // Adds a file, or every audio file under a directory (sorted). Returns count added.
    size_t add(const std::filesystem::path &path);
    void clear();

    bool empty() const { return tracks_.empty(); }
    size_t size() const { return tracks_.size(); }
    int index() const { return current_; }
    const std::filesystem::path &current() const { return tracks_[size_t(current_)]; }

    // Picking a track by hand also records the current one, so Previous can return to it.
    void select(int i);
    const std::vector<std::filesystem::path> &tracks() const { return tracks_; }
    // Removing the current track leaves `index()` just before the track that
    // takes its place, so next() continues from there.
    void remove(int i);
    // Moves track i by `delta` places; returns its new index.
    int move(int i, int delta);
    // Starts over at the first track (a random one when shuffling), forgetting the history.
    void start();
    // Moves to the next/previous track; returns false at either end without repeat.
    bool next();
    bool prev();

    bool shuffle = false;
    bool repeat = true;
    // Shuffle: a track that played waits until this share of the playlist has played since
    // (the most recent ones queue up, the oldest trickles back into the pool with each pick).
    float gap = 0.33f;

private:
    bool nextShuffled();
    bool nextInOrder();

    std::vector<std::filesystem::path> tracks_;
    std::vector<int> history_; // tracks played before the current one (most recent last)
    std::vector<int> future_;  // tracks Previous stepped back from (Next returns to them, most recent last)
    void remember();
    int current_ = -1;
    std::mt19937 rng_{std::random_device{}()};
};

bool isAudioFile(const std::filesystem::path &p);
