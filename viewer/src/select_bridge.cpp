// The engine's frame selection behind a C ABI, for a browser that decodes the
// video itself; js/ss_select.d.ts is the contract. Every decision is the
// engine's own code, compiled in place from src/app: the motion pass and its
// plan, the sharpness window, and the rule that leaves a chosen frame out.
// A result lives in memory the module owns until the next call on the same
// object, or of the same function where there is no object.

#include "app/FrameMotion.h"
#include "app/FramePick.h"
#include "app/FrameSharpness.h"
#include "app/SelectClock.h"

#include <emscripten.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// The layouts below are read field by field from JavaScript (ss_select.d.ts).
struct Options {
    int32_t view, width, height;
    float circle_x, circle_y, circle_r, circle_fov, out_fov;
};

struct PlanInput {
    int32_t frames, span, skip, window, max_frames, view;
    double fps;
    float out_fov, range;
};

struct Verdict {
    int32_t index;
    float width, blur;
    uint32_t hash_lo, hash_hi;
    int32_t verdict;
};

static_assert(sizeof(app::MotionStep) == 7 * sizeof(float), "a step is read as 7 floats");
static_assert(offsetof(PlanInput, fps) == 24 && sizeof(PlanInput) == 40, "PlanInput layout");
static_assert(sizeof(Options) == 32 && sizeof(Verdict) == 24, "record layouts");

struct Tracker {
    app::MotionOptions options;
    app::MotionTracker tracker;
    Options flat;
    std::vector<int32_t> ends;
    explicit Tracker(const app::MotionOptions& o) : options(o), tracker(o) {}
};

struct Clock {
    app::SelectClock clock;
    app::WindowBest top;
};

struct Pick {
    app::PickRule rule;
    std::unique_ptr<app::EdgeMeter> meter;
    Verdict last{};
};

Options g_options;
PlanInput g_plan_input;
std::vector<int32_t> g_plan;
uint32_t g_hash[2];

// A source the browser decodes has no 360 packing this module knows: what the
// engine makes of tracks it cannot lay out as one.
app::MotionOptions options_for(int tracks, int src_w, int src_h) {
    return app::motion_options(app::Pano360Layout{}, {}, tracks, src_w, src_h);
}

Options flatten(const app::MotionOptions& o) {
    return {(int32_t)o.view, o.width,      o.height,   o.circle_x,
            o.circle_y,      o.circle_r,   o.circle_fov, o.out_fov};
}

// As extract_frames plans: one input, on its own budget.
int run_plan(const app::MotionPlanInput& in, float range) {
    const std::vector<std::vector<int64_t>> got = app::plan_by_motion({in}, range);
    g_plan.clear();
    if (!got.empty())
        for (int64_t e : got[0]) g_plan.push_back((int32_t)e);
    g_plan_input = {(int32_t)in.frames, (int32_t)in.span, in.skip,   in.window, in.max_frames,
                    (int32_t)in.view,   in.fps,           in.out_fov, range};
    return (int)g_plan.size();
}

}  // namespace

extern "C" {

// ================
// The motion pass
// ================

EMSCRIPTEN_KEEPALIVE const Options* sss_motion_options(int tracks, int src_w, int src_h) {
    g_options = flatten(options_for(tracks, src_w, src_h));
    return &g_options;
}

EMSCRIPTEN_KEEPALIVE int sss_motion_stride(double fps, int skip) {
    return app::motion_stride(fps, skip);
}

EMSCRIPTEN_KEEPALIVE void sss_luma_box(const uint8_t* y, int sw, int sh, int ow, int oh,
                                       uint8_t* out) {
    app::luma_box(y, sw, sh, ow, oh, out);
}

EMSCRIPTEN_KEEPALIVE Tracker* sss_tracker_new(int tracks, int src_w, int src_h) {
    Tracker* t = new Tracker(options_for(tracks, src_w, src_h));
    t->flat = flatten(t->options);
    return t;
}

EMSCRIPTEN_KEEPALIVE const Options* sss_tracker_options(const Tracker* t) { return &t->flat; }

EMSCRIPTEN_KEEPALIVE void sss_tracker_track(Tracker* t, const uint8_t* gray, int index) {
    t->tracker.track(gray, index);
}

EMSCRIPTEN_KEEPALIVE void sss_tracker_finish(Tracker* t) { t->tracker.finish(); }

EMSCRIPTEN_KEEPALIVE int sss_tracker_count(const Tracker* t) {
    return (int)t->tracker.costs().size();
}

EMSCRIPTEN_KEEPALIVE const float* sss_tracker_costs(const Tracker* t) {
    return t->tracker.costs().data();
}

EMSCRIPTEN_KEEPALIVE const float* sss_tracker_steps(const Tracker* t) {
    return reinterpret_cast<const float*>(t->tracker.steps().data());
}

EMSCRIPTEN_KEEPALIVE const int32_t* sss_tracker_ends(Tracker* t) {
    const std::vector<int64_t>& e = t->tracker.ends();
    t->ends.assign(e.begin(), e.end());
    return t->ends.data();
}

EMSCRIPTEN_KEEPALIVE int sss_tracker_weak_steps(const Tracker* t) {
    return t->tracker.weak_steps();
}

EMSCRIPTEN_KEEPALIVE int sss_tracker_plan(Tracker* t, int frames, int span_first, int span_end,
                                          int skip, int keep, int max_frames, double fps,
                                          float range) {
    app::FrameSpan span;
    span.first = span_first;
    span.end = span_end;
    return run_plan(app::motion_plan_input(t->tracker, t->options, frames, span, skip, keep,
                                           max_frames, fps),
                    range);
}

EMSCRIPTEN_KEEPALIVE void sss_tracker_free(Tracker* t) { delete t; }

// ================
// The plan
// ================

EMSCRIPTEN_KEEPALIVE int sss_plan(const float* cost, const float* steps, const int32_t* ends,
                                  int n, int frames, int span, int skip, int window,
                                  int max_frames, double fps, int view, float out_fov,
                                  float range) {
    app::MotionPlanInput in;
    in.cost.assign(cost, cost + n);
    in.ends.assign(ends, ends + n);
    if (steps) {
        in.step.resize((size_t)n);
        std::memcpy(in.step.data(), steps, (size_t)n * sizeof(app::MotionStep));
    }
    in.frames = frames;
    in.span = span;
    in.skip = skip;
    in.window = window;
    in.max_frames = max_frames;
    in.fps = fps;
    in.view = (app::MotionView)view;
    in.out_fov = out_fov;
    return run_plan(in, range);
}

EMSCRIPTEN_KEEPALIVE const int32_t* sss_plan_ends() { return g_plan.data(); }

EMSCRIPTEN_KEEPALIVE const PlanInput* sss_plan_input() { return &g_plan_input; }

// ================
// The sharpness window
// ================

EMSCRIPTEN_KEEPALIVE int sss_window_keep(int keep, int skip) {
    return app::window_keep(keep, skip);
}

EMSCRIPTEN_KEEPALIVE Clock* sss_clock_new(int keep, int skip, const int32_t* ends, int n,
                                          int span_first, int span_end) {
    Clock* c = new Clock;
    c->clock.keep = keep;
    c->clock.skip = skip;
    c->clock.ends.assign(ends, ends + n);
    c->clock.span.first = span_first;
    c->clock.span.end = span_end;
    return c;
}

EMSCRIPTEN_KEEPALIVE int sss_clock_candidate(Clock* c, int index) {
    return c->clock.candidate(index) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int sss_clock_offer(Clock* c, double score) {
    return c->top.offer(score) ? 1 : 0;
}

// Both decoders start a window's pick afresh once the last one closes.
EMSCRIPTEN_KEEPALIVE int sss_clock_step(Clock* c, int index) {
    if (!c->clock.step(index)) return 0;
    c->top = app::WindowBest{};
    return 1;
}

EMSCRIPTEN_KEEPALIVE void sss_clock_free(Clock* c) { delete c; }

EMSCRIPTEN_KEEPALIVE double sss_sharpness_score(const uint8_t* rgb, int width, int height) {
    return app::sharpness_score(rgb, width, height);
}

// ================
// Leaving chosen frames out
// ================

EMSCRIPTEN_KEEPALIVE float sss_edge_width_px(const uint8_t* rgb, int width, int height) {
    return app::edge_width_px(rgb, width, height);
}

EMSCRIPTEN_KEEPALIVE const uint32_t* sss_frame_dhash(const uint8_t* rgb, int width,
                                                     int height) {
    const uint64_t h = app::frame_dhash(rgb, width, height);
    g_hash[0] = (uint32_t)h;
    g_hash[1] = (uint32_t)(h >> 32);
    return g_hash;
}

// The decoders meter edge widths only where blur is judged.
EMSCRIPTEN_KEEPALIVE Pick* sss_pick_new(float max_blur, int neighbour, int similar_bits,
                                        float lens) {
    Pick* p = new Pick;
    p->rule.max_blur = max_blur;
    p->rule.neighbour = neighbour != 0;
    p->rule.similar_bits = similar_bits;
    p->rule.lens = lens;
    if (max_blur > 0.0f) p->meter = std::make_unique<app::EdgeMeter>();
    return p;
}

EMSCRIPTEN_KEEPALIVE int sss_pick_on(const Pick* p) { return p->rule.on() ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE const Verdict* sss_pick_measure(Pick* p, const uint8_t* rgb, int width,
                                                     int height) {
    app::FramePick fp;
    app::measure_pick(fp, rgb, width, height, p->meter.get());
    p->last = {-1, fp.width, -1.0f, (uint32_t)fp.hash, (uint32_t)(fp.hash >> 32), 0};
    return &p->last;
}

EMSCRIPTEN_KEEPALIVE int sss_pick_judge(Pick* p, int index, float width, uint32_t hash_lo,
                                        uint32_t hash_hi) {
    app::FramePick fp;
    fp.index = index;
    fp.width = width;
    fp.hash = (uint64_t)hash_hi << 32 | hash_lo;
    p->rule.judge(fp);
    p->last = {index, fp.width, fp.blur, hash_lo, hash_hi, (int32_t)fp.verdict};
    return (int)fp.verdict;
}

EMSCRIPTEN_KEEPALIVE const Verdict* sss_pick_last(const Pick* p) { return &p->last; }

EMSCRIPTEN_KEEPALIVE float sss_pick_lens_width(const float* widths, int n) {
    return app::PickRule::lens_width(std::vector<float>(widths, widths + n));
}

EMSCRIPTEN_KEEPALIVE void sss_pick_free(Pick* p) { delete p; }

}  // extern "C"
