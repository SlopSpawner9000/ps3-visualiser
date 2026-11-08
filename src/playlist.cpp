#include "playlist.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fs = std::filesystem;

bool isAudioFile(const fs::path &p)
{
    static const char *exts[] = {".mp3", ".flac", ".ogg", ".opus", ".wav", ".m4a", ".aac", ".wma",
                                 ".aiff", ".aif", ".ape", ".wv", ".mka", ".ac3", ".at3", ".oma", ".alac", ".mpc"};
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(std::begin(exts), std::end(exts), [&](const char *x) { return e == x; });
}

size_t Playlist::add(const fs::path &path)
{
    std::error_code ec;
    std::vector<fs::path> found;
    if (fs::is_directory(path, ec)) {
        for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            if (it->is_regular_file(ec) && isAudioFile(it->path()))
                found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
    } else if (fs::is_regular_file(path, ec)) {
        found.push_back(path);
    }
    tracks_.insert(tracks_.end(), found.begin(), found.end());
    return found.size();
}

void Playlist::remember()
{
    if (current_ >= 0 && (history_.empty() || history_.back() != current_))
        history_.push_back(current_);
    if (history_.size() > 1000)
        history_.erase(history_.begin());
}

void Playlist::select(int i)
{
    if (i != current_)
        remember();
    current_ = i;
    future_.clear();
}

void Playlist::start()
{
    current_ = -1;
    history_.clear();
    future_.clear();
    next();
}

void Playlist::clear()
{
    tracks_.clear();
    history_.clear();
    future_.clear();
    current_ = -1;
}

bool Playlist::next()
{
    if (tracks_.empty())
        return false;
    const int before = current_;
    const bool moved = shuffle ? nextShuffled() : nextInOrder();
    if (moved && current_ != before && before >= 0) {
        const int now = current_;
        current_ = before;
        remember();
        current_ = now;
    }
    return moved;
}

bool Playlist::nextShuffled()
{
    // Back where Previous came from first.
    while (!future_.empty()) {
        const int f = future_.back();
        future_.pop_back();
        if (f >= 0 && f < int(tracks_.size())) {
            current_ = f;
            return true;
        }
    }
    // Waiting: the current track and the ones before it, back to `gap` of the playlist.
    // Without repeat, a track never comes back (the playlist ends once every track has played).
    const size_t n = tracks_.size();
    std::vector<char> waiting(n, 0);
    size_t keep = std::min(n - 1, size_t(std::lround(double(std::clamp(gap, 0.0f, 0.9f)) * double(n))));
    if (current_ >= 0 && current_ < int(n))
        waiting[size_t(current_)] = 1;
    if (!repeat) {
        for (int h : history_)
            if (h >= 0 && h < int(n))
                waiting[size_t(h)] = 1;
    } else {
        for (auto it = history_.rbegin(); it != history_.rend() && keep > 0; ++it)
            if (*it >= 0 && *it < int(n) && !waiting[size_t(*it)]) {
                waiting[size_t(*it)] = 1;
                keep--;
            }
    }
    std::vector<int> pick;
    for (int i = 0; i < int(n); i++)
        if (!waiting[size_t(i)])
            pick.push_back(i);
    if (pick.empty()) {
        if (!repeat)
            return false;
        pick.push_back(std::max(current_, 0)); // a single track
    }
    current_ = pick[std::uniform_int_distribution<size_t>(0, pick.size() - 1)(rng_)];
    return true;
}

bool Playlist::nextInOrder()
{
    if (current_ + 1 >= int(tracks_.size())) {
        if (!repeat)
            return false;
        current_ = 0;
        return true;
    }
    current_++;
    return true;
}

bool Playlist::prev()
{
    if (tracks_.empty())
        return false;
    // Shuffle: go back to the track that actually played before this one.
    if (shuffle) {
        while (!history_.empty() && (history_.back() < 0 || history_.back() >= int(tracks_.size())))
            history_.pop_back();
        if (!history_.empty()) {
            future_.push_back(current_);
            current_ = history_.back();
            history_.pop_back();
            return true;
        }
    }
    current_ = current_ <= 0 ? (repeat ? int(tracks_.size()) - 1 : 0) : current_ - 1;
    return true;
}

void Playlist::remove(int i)
{
    if (i < 0 || i >= int(tracks_.size()))
        return;
    tracks_.erase(tracks_.begin() + i);
    // Keep the play history pointing at the same tracks.
    for (auto *list : {&history_, &future_}) {
        std::vector<int> kept;
        for (int h : *list)
            if (h != i)
                kept.push_back(h > i ? h - 1 : h);
        list->swap(kept);
    }
    if (i <= current_)
        current_--;
    if (tracks_.empty())
        current_ = -1;
}

int Playlist::move(int i, int delta)
{
    const int j = i + delta;
    if (i < 0 || j < 0 || i >= int(tracks_.size()) || j >= int(tracks_.size()))
        return i;
    std::swap(tracks_[size_t(i)], tracks_[size_t(j)]);
    for (auto *list : {&history_, &future_})
        for (int &h : *list)
            h = h == i ? j : h == j ? i : h;
    if (current_ == i)
        current_ = j;
    else if (current_ == j)
        current_ = i;
    return j;
}
