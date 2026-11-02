#pragma once

// Which decoded frames are candidates, when a window of them closes, and which
// of a window's candidates is kept. Both decoders (app/FrameDecode.h) and the
// browser's module (viewer/src/select_bridge.cpp) walk these, so the frames
// kept follow from the scores alone, not from who decoded them.

#include "app/FrameClip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace app {

// The window a `keep` below zero asks for: half the spacing, rounded.
inline int window_keep(int keep, int skip) {
    return keep >= 0 ? keep : (int)(0.5 * skip + 0.5);
}

// Which decoded frames are candidates and when a window is written.
struct SelectClock {
    // A fixed rate is arithmetic on the index; `ends`, from the motion pass,
    // is the frame each window closes at. Both are walked forward only.
    int keep = 0, skip = 1;
    std::vector<int64_t> ends;
    size_t next = 0;
    FrameSpan span;

    bool candidate(int64_t i) {
        if (i < span.first || span.past(i)) return false;
        if (!ends.empty()) {
            while (next < ends.size() && ends[next] < i) ++next;
            return next < ends.size() && i > ends[next] - std::max(keep, 1) &&
                   i <= ends[next];
        }
        return keep == 0 ? (i % skip == 0) : (((i + keep) % skip) < keep);
    }
    bool step(int64_t i) {
        if (!ends.empty()) return next < ends.size() && i == ends[next];
        return keep == 0 || ((i + 1) % skip) == 0;
    }
};

// A window's pick: its highest score, the first of equal ones. A score is a
// variance, so the floor is under every one and the first candidate is held.
struct WindowBest {
    double score = -1.0;
    // True when `s` beats every earlier candidate of the window: hold this one.
    bool offer(double s) {
        if (!(s > score)) return false;
        score = s;
        return true;
    }
};

}  // namespace app
