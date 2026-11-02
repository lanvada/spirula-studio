// select_parity -- the frame selection replayed natively from the fixture the
// browser module is tested on (viewer/test/select.test.mjs), held to the same
// expected file. That file is written from the WASM build; this run agreeing
// with it is what says the module's flags and float semantics make the
// engine's decisions. Prints what it got as JSON either way.
//
//   select_parity_test viewer/test/select-fixture.json
//                      viewer/test/select-fixture.expected.json

#include "app/FrameMotion.h"
#include "app/FramePick.h"
#include "app/SelectClock.h"
#include "data/Json.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "BAD  %s\n", what.c_str());
        g_failures++;
    }
}

const JsonValue& at(const JsonValue& v, const char* key) {
    static const JsonValue none;
    const JsonValue* f = v.find(key);
    return f ? *f : none;
}

std::vector<int64_t> ints(const JsonValue& v) {
    std::vector<int64_t> out;
    for (const JsonValue& x : v.arr) out.push_back(x.as_int());
    return out;
}

// A JSON number holds a float exactly, so equal here is equal to the bit.
bool same(const JsonValue& want, double got) { return want.as_double() == got; }

void put_ints(const char* key, const std::vector<int64_t>& v, bool comma = true) {
    std::printf("\"%s\": [", key);
    for (size_t i = 0; i < v.size(); i++) std::printf("%s%lld", i ? ", " : "", (long long)v[i]);
    std::printf("]%s", comma ? ", " : "");
}

app::MotionPlanInput plan_input(const JsonValue& m) {
    app::MotionPlanInput in;
    for (const JsonValue& s : at(m, "steps").arr) {
        in.cost.push_back((float)at(s, "cost").as_double());
        in.ends.push_back(at(s, "end").as_int());
        app::MotionStep step;
        for (int k = 0; k < 6; k++) step.turn[k] = (float)at(s, "turn").arr[(size_t)k].as_double();
        step.drift = (float)at(s, "drift").as_double();
        in.step.push_back(step);
    }
    in.frames = at(m, "frames").as_int();
    in.span = at(m, "span").as_int();
    in.skip = (int)at(m, "skip").as_int();
    in.window = (int)at(m, "window").as_int();
    in.max_frames = (int)at(m, "max_frames").as_int();
    in.fps = at(m, "fps").as_double();
    const std::string& view = at(m, "view").as_string();
    in.view = view == "fisheye"     ? app::MotionView::Fisheye
              : view == "packed360" ? app::MotionView::Packed360
                                    : app::MotionView::Planar;
    in.out_fov = (float)at(m, "out_fov").as_double();
    return in;
}

uint64_t parse_hash(const std::string& hex) { return std::stoull(hex, nullptr, 16); }

// decode_lockstep_ffmpeg's loop over every frame of the clip, with the
// fixture's numbers where the decoder would measure pixels.
void replay(const JsonValue& c, const JsonValue& want) {
    const std::string name = at(c, "name").as_string();
    std::vector<int64_t> plan;
    const JsonValue& motion = at(c, "motion");
    if (!motion.is_null()) {
        const std::vector<std::vector<int64_t>> got = app::plan_by_motion(
            {plan_input(motion)}, (float)at(motion, "adaptive_range").as_double());
        if (!got.empty()) plan = got[0];
    }
    const int keep = (int)at(c, "keep").as_int(), skip = (int)at(c, "skip").as_int();
    const int max_frames = (int)at(c, "max_frames").as_int();
    const int64_t frames = at(c, "frames").as_int();
    app::SelectClock clock{keep, skip, plan, 0};
    clock.span.first = at(c, "span").arr[0].as_int();
    clock.span.end = at(c, "span").arr[1].as_int();
    app::WindowBest top;

    const JsonValue& pick = at(c, "pick");
    app::PickRule rule;
    rule.max_blur = (float)at(pick, "max_blur").as_double();
    rule.neighbour = at(pick, "neighbour").as_bool();
    rule.similar_bits = (int)at(pick, "similar_bits").as_int();
    rule.lens = (float)at(pick, "lens").as_double();
    const bool judge = rule.on();

    struct Chosen {
        int64_t index;
        int verdict;
        float blur;
    };
    std::vector<Chosen> chosen;
    std::vector<int64_t> kept;
    int64_t held = -1;
    const int64_t end = clock.span.end >= 0 ? std::min(clock.span.end, frames) : frames;
    for (int64_t i = clock.span.first; i < end; i++) {
        if (max_frames > 0 && (int)kept.size() >= max_frames) break;
        if (!clock.candidate(i)) continue;
        if (top.offer(keep != 0 ? at(c, "scores").arr[(size_t)i].as_double() : 0.0)) held = i;
        if (!clock.step(i)) continue;
        top = app::WindowBest{};
        app::FramePick p;
        p.index = held;
        if (judge) {
            p.hash = parse_hash(at(c, "hashes").arr[(size_t)held].as_string());
            if (rule.max_blur > 0.0f) p.width = (float)at(c, "widths").arr[(size_t)held].as_double();
            rule.judge(p);
        }
        chosen.push_back({held, (int)p.verdict, judge ? p.blur : -1.0f});
        if (p.verdict == app::PickVerdict::Kept) kept.push_back(held);
    }

    std::printf("    {\"name\": \"%s\", ", name.c_str());
    put_ints("plan", plan);
    std::printf("\"chosen\": [");
    for (size_t k = 0; k < chosen.size(); k++)
        std::printf("%s[%lld, %d, %.9g]", k ? ", " : "", (long long)chosen[k].index,
                    chosen[k].verdict, chosen[k].blur);
    std::printf("], ");
    put_ints("kept", kept, false);
    std::printf("}");

    expect(ints(at(want, "plan")) == plan, name + ": plan");
    expect(ints(at(want, "kept")) == kept, name + ": kept");
    const std::vector<JsonValue>& w = at(want, "chosen").arr;
    bool same_chosen = w.size() == chosen.size();
    for (size_t k = 0; same_chosen && k < w.size(); k++)
        same_chosen = w[k].arr[0].as_int() == chosen[k].index &&
                      w[k].arr[1].as_int() == chosen[k].verdict && same(w[k].arr[2], chosen[k].blur);
    expect(same_chosen, name + ": chosen frames, verdicts and blur");
}

// The texture viewer/test/select.test.mjs draws, to the bit: value noise on an
// 8 px lattice plus a fine grain, in unsigned 32-bit arithmetic.
uint32_t mix(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x9e3779b1u ^ y * 0x85ebca77u ^ seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

uint8_t texel(int u, int v, uint32_t seed) {
    const int cx = u >> 3, cy = v >> 3, fx = u & 7, fy = v & 7;
    const int top = (int)(mix(cx, cy, seed) & 255) * (8 - fx) + (int)(mix(cx + 1, cy, seed) & 255) * fx;
    const int bot =
        (int)(mix(cx, cy + 1, seed) & 255) * (8 - fx) + (int)(mix(cx + 1, cy + 1, seed) & 255) * fx;
    const int v8 = ((top * (8 - fy) + bot * fy) >> 6) + (int)(mix(u, v, seed ^ 0x5bd1e995u) & 15) - 8;
    return (uint8_t)std::min(255, std::max(0, v8));
}

void track(const JsonValue& t, const JsonValue& want) {
    const int sw = (int)at(t, "source").arr[0].as_int(), sh = (int)at(t, "source").arr[1].as_int();
    const int stride = (int)at(t, "stride").as_int();
    const uint32_t seed = (uint32_t)at(t, "seed").as_int();
    const app::MotionOptions mo =
        app::motion_options(app::Pano360Layout{}, {}, (int)at(t, "tracks").as_int(), sw, sh);
    app::MotionTracker tracker(mo);
    std::vector<uint8_t> y((size_t)sw * sh), grey((size_t)mo.width * mo.height);
    const std::vector<JsonValue>& offsets = at(t, "offsets").arr;
    for (size_t k = 0; k < offsets.size(); k++) {
        const int ox = (int)offsets[k].arr[0].as_int(), oy = (int)offsets[k].arr[1].as_int();
        for (int r = 0; r < sh; r++)
            for (int c = 0; c < sw; c++)
                y[(size_t)r * sw + c] = texel(c + ox + 1000, r + oy + 1000, seed);
        app::luma_box(y.data(), sw, sh, mo.width, mo.height, grey.data());
        tracker.track(grey.data(), (int64_t)k * stride);
    }
    tracker.finish();

    const int skip = (int)at(t, "skip").as_int();
    const app::MotionPlanInput in = app::motion_plan_input(
        tracker, mo, (int64_t)offsets.size() * stride, app::FrameSpan{}, skip,
        app::window_keep((int)at(t, "keep").as_int(), skip), 0, at(t, "fps").as_double());
    const std::vector<std::vector<int64_t>> got =
        app::plan_by_motion({in}, (float)at(t, "range").as_double());
    const std::vector<int64_t> plan = got.empty() ? std::vector<int64_t>() : got[0];

    const std::vector<float>& costs = tracker.costs();
    const std::vector<app::MotionStep>& steps = tracker.steps();
    std::printf("  \"track\": {\"view\": %d, \"grey\": [%d, %d], \"weak\": %d, ", (int)mo.view,
                mo.width, mo.height, tracker.weak_steps());
    put_ints("ends", tracker.ends());
    std::printf("\"costs\": [");
    for (size_t k = 0; k < costs.size(); k++) std::printf("%s%.9g", k ? ", " : "", costs[k]);
    std::printf("], ");
    put_ints("plan", plan, false);
    std::printf("}\n");

    expect(at(want, "view").as_int() == (int)mo.view, "track: view");
    expect(at(want, "grey").arr.size() == 2 && at(want, "grey").arr[0].as_int() == mo.width &&
               at(want, "grey").arr[1].as_int() == mo.height,
           "track: grey frame size");
    expect(at(want, "weak").as_int() == tracker.weak_steps(), "track: weak steps");
    expect(ints(at(want, "ends")) == tracker.ends(), "track: ends");
    expect(ints(at(want, "plan")) == plan, "track: plan");
    const std::vector<JsonValue>& wc = at(want, "costs").arr;
    const std::vector<JsonValue>& ws = at(want, "steps").arr;
    bool same_steps = wc.size() == costs.size() && ws.size() == steps.size();
    for (size_t k = 0; same_steps && k < costs.size(); k++) {
        same_steps = same(wc[k], costs[k]) && ws[k].arr.size() == 7 &&
                     same(ws[k].arr[6], steps[k].drift);
        for (int j = 0; same_steps && j < 6; j++) same_steps = same(ws[k].arr[(size_t)j], steps[k].turn[j]);
    }
    expect(same_steps, "track: every step's cost, turn and drift");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <select-fixture.json> <select-fixture.expected.json>\n",
                     argv[0]);
        return 2;
    }
    try {
        const JsonValue fixture = json_parse_file(argv[1]);
        const JsonValue expected = json_parse_file(argv[2]);
        const std::vector<JsonValue>& cases = at(fixture, "cases").arr;
        const std::vector<JsonValue>& want = at(expected, "cases").arr;
        expect(cases.size() == want.size(), "as many expected cases as cases");
        std::printf("{\n  \"cases\": [\n");
        for (size_t k = 0; k < cases.size() && k < want.size(); k++) {
            replay(cases[k], want[k]);
            std::printf("%s\n", k + 1 < cases.size() ? "," : "");
        }
        std::printf("  ],\n");
        track(at(fixture, "track"), at(expected, "track"));
        std::printf("}\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "select_parity_test: %s\n", e.what());
        return 2;
    }
    std::fprintf(stderr, "%s\n", g_failures ? "select_parity: FAILED" : "select_parity: ok");
    return g_failures ? 1 : 0;
}
