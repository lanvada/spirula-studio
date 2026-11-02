// ss_select.js / ss_select.wasm -- `spirula sam extract`'s frame selection for a
// browser that decodes the video itself, built from src/app in place (target
// ss_select in viewer/CMakeLists.txt; the C side is viewer/src/select_bridge.cpp).
// A pointer is a byte offset into the module's memory: read it through the heap
// views, and fetch the view again after any call that allocates, since growing
// memory replaces every view. Input buffers are the caller's (_malloc, fill,
// _free); a returned pointer is the module's, valid until the next call on the
// same object, or of the same function where there is no object, or until the
// object is freed. Frame indices are source frame numbers from 0. Images are
// tightly packed rows: RGB8, or one byte a pixel for a Y plane or grey frame.

/** Byte offset into the module's memory. */
export type Ptr = number;

/** MotionView: 0 planar, 1 packed360, 2 fisheye. */
export type View = 0 | 1 | 2;

/** PickVerdict: 0 kept, 1 blurry, 2 similar. */
export type Verdict = 0 | 1 | 2;

/**
 * 32 bytes: int32 view (View), int32 width, int32 height -- the grey frame the
 * tracker takes; float32 circle_x, circle_y, circle_r -- the fisheye image
 * circle in grey pixels, 0 for the inscribed one; float32 circle_fov -- radians
 * across that circle; float32 out_fov -- radians across the diagonal of the
 * views a dataset is cut into, which an angular residual is a fraction of.
 */
export type OptionsPtr = Ptr;

/**
 * 28 bytes a step: float32 turn[6] -- the 2x3 affine the frame was carried
 * over by, in unit-square frame coordinates, identity on a sphere; float32
 * drift -- the parallax cost, which no model composes.
 */
export type StepsPtr = Ptr;

/**
 * 40 bytes: int32 frames, span, skip, window, max_frames, view (View) at bytes
 * 0-23; float64 fps at 24; float32 out_fov at 32; float32 range at 36.
 */
export type PlanInputPtr = Ptr;

/**
 * 24 bytes: int32 index; float32 width -- edge width in pixels, negative
 * unmeasured; float32 blur -- pixels, negative unmeasured; uint32 hash_lo,
 * hash_hi -- the 64-bit difference hash; int32 verdict (Verdict).
 */
export type PickPtr = Ptr;

/** Objects the module allocates; free each with its own _free call. */
export type Tracker = Ptr;
export type Clock = Ptr;
export type Rule = Ptr;

export interface SSSelect {
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
  HEAPU32: Uint32Array;
  HEAPF32: Float32Array;
  HEAPF64: Float64Array;
  _malloc(bytes: number): Ptr;
  _free(ptr: Ptr): void;
  ccall(name: string, returnType: string | null, argTypes: string[], args: unknown[]): unknown;
  cwrap(name: string, returnType: string | null, argTypes: string[]): (...args: unknown[]) => unknown;

  // ---- the motion pass, run only for an adaptive plan ----

  /**
   * What a source is measured as: `tracks` video tracks of `srcW` x `srcH`
   * source pixels, unturned. Two or more square tracks are a dual-fisheye rig;
   * a 360 packing (GoPro .360) is not known to this module.
   */
  _sss_motion_options(tracks: number, srcW: number, srcH: number): OptionsPtr;
  /**
   * Every how many source frames the motion pass samples: the frames whose
   * index % stride == 0. `fps` is the source's rate, 30 where the container
   * gives none.
   */
  _sss_motion_stride(fps: number, skip: number): number;
  /**
   * A decoded Y plane, `sw` x `sh` and unturned, boxed into the `ow` x `oh` grey
   * frame at `out`. Both decoders feed the tracker this from the Y plane, so
   * hand it the decoder's Y plane (VideoFrame.copyTo), not luma made from RGB.
   */
  _sss_luma_box(y: Ptr, sw: number, sh: number, ow: number, oh: number, out: Ptr): void;

  /** A tracker over the source's first track, measuring as _sss_motion_options says. */
  _sss_tracker_new(tracks: number, srcW: number, srcH: number): Tracker;
  _sss_tracker_options(tracker: Tracker): OptionsPtr;
  /** One grey frame (width x height of the options) of source frame `index`; the first only primes. */
  _sss_tracker_track(tracker: Tracker, gray: Ptr, index: number): void;
  /** Once, after the last frame: fills the steps nothing could be tracked across. The tables are final only after it. */
  _sss_tracker_finish(tracker: Tracker): void;
  /** Steps: one fewer than the frames tracked. */
  _sss_tracker_count(tracker: Tracker): number;
  /** float32[count]: each step's view change. */
  _sss_tracker_costs(tracker: Tracker): Ptr;
  _sss_tracker_steps(tracker: Tracker): StepsPtr;
  /** int32[count]: the source frame each step ends at. */
  _sss_tracker_ends(tracker: Tracker): Ptr;
  /** Steps whose flow was too weak to fit a model to. */
  _sss_tracker_weak_steps(tracker: Tracker): number;
  /**
   * The plan from this tracker's steps, as extract_frames makes it: `frames` in
   * the video, the clip [spanFirst, spanEnd) read (0, -1 for all of it), `keep`
   * as _sss_window_keep resolves it. Returns how many windows there are; their
   * ends are at _sss_plan_ends, the input at _sss_plan_input. 0 is a capture
   * too short to space by motion, which `spirula sam extract` fails on.
   */
  _sss_tracker_plan(tracker: Tracker, frames: number, spanFirst: number, spanEnd: number,
                    skip: number, keep: number, maxFrames: number, fps: number,
                    range: number): number;
  _sss_tracker_free(tracker: Tracker): void;

  // ---- the plan ----

  /**
   * The plan from a recorded motion table (`spirula sam extract
   * --motion-report`): float32 cost[n], steps[n] (or 0 to space on cost alone),
   * int32 ends[n], and the report's scalars. Returns the count, as
   * _sss_tracker_plan does.
   */
  _sss_plan(cost: Ptr, steps: StepsPtr, ends: Ptr, n: number, frames: number, span: number,
            skip: number, window: number, maxFrames: number, fps: number, view: View,
            outFov: number, range: number): number;
  /** int32[count] of the last plan: the source frame each window closes at. */
  _sss_plan_ends(): Ptr;
  _sss_plan_input(): PlanInputPtr;

  // ---- the sharpness window ----

  /** The window a `keep` below zero means, round(skip / 2); any other value is itself. */
  _sss_window_keep(keep: number, skip: number): number;
  /**
   * Which decoded frames are candidates and where a window closes: `keep` as
   * _sss_window_keep resolves it, `skip` >= 1, the plan's int32 ends[n] (n = 0
   * for a fixed rate), and the clip [spanFirst, spanEnd), -1 for no end. Walk
   * it forward over every decoded frame index, never back.
   */
  _sss_clock_new(keep: number, skip: number, ends: Ptr, n: number, spanFirst: number,
                 spanEnd: number): Clock;
  /** 1 when frame `index` is a candidate. */
  _sss_clock_candidate(clock: Clock, index: number): number;
  /**
   * A candidate's score, summed over the instant's tracks; 0 when keep is 0.
   * 1 when it beats every earlier candidate of the window (the first of equal
   * ones stays): hold this instant's pictures.
   */
  _sss_clock_offer(clock: Clock, score: number): number;
  /**
   * 1 when the window closes at `index`: what is held is its pick, and the next
   * window starts afresh. Call after _sss_clock_offer for the same frame.
   */
  _sss_clock_step(clock: Clock, index: number): number;
  _sss_clock_free(clock: Clock): void;
  /** The ffmpeg decoder's score of an RGB8 picture: variance of the Laplacian of a 512x512 grey box. */
  _sss_sharpness_score(rgb: Ptr, width: number, height: number): number;

  // ---- leaving a pick out ----

  /** Edge width of an RGB8 picture in its own pixels; negative where too few edges were found. */
  _sss_edge_width_px(rgb: Ptr, width: number, height: number): number;
  /** uint32[2], lo then hi: the picture's 64-bit difference hash; 0 for one too small to hash. */
  _sss_frame_dhash(rgb: Ptr, width: number, height: number): Ptr;
  /**
   * The rule `--blur-reject <maxBlur>` and `--similar <similarBits>` set (0 is
   * off); `neighbour` 1 refuses at most two in a row; `lens` is the capture's
   * own edge width, or -1 to take it from the frames judged so far.
   */
  _sss_pick_new(maxBlur: number, neighbour: number, similarBits: number, lens: number): Rule;
  /** 1 when the rule judges at all; with 0 every pick is kept. */
  _sss_pick_on(rule: Rule): number;
  /**
   * What the rule reads off a pick's RGB8, the first track's picture: the hash,
   * and the edge width only where blur is judged (negative otherwise).
   */
  _sss_pick_measure(rule: Rule, rgb: Ptr, width: number, height: number): PickPtr;
  /** Rules on one window's pick, picks in order; its blur is in the record _sss_pick_last returns. */
  _sss_pick_judge(rule: Rule, index: number, width: number, hashLo: number,
                  hashHi: number): Verdict;
  _sss_pick_last(rule: Rule): PickPtr;
  /** The `lens` a whole capture gives: PickRule::lens_width over float32 widths[n]. */
  _sss_pick_lens_width(widths: Ptr, n: number): number;
  _sss_pick_free(rule: Rule): void;
}

export interface SSSelectModuleArg {
  /** Where ss_select.wasm is, given its name and the directory ss_select.js was loaded from. */
  locateFile?(path: string, scriptDirectory: string): string;
  /** Instantiate the module yourself, e.g. from bytes already fetched, and hand `done` the instance. */
  instantiateWasm?(
    imports: WebAssembly.Imports,
    done: (instance: WebAssembly.Instance) => void,
  ): WebAssembly.Exports | Record<string, never>;
}

export default function SSSelectModule(arg?: SSSelectModuleArg): Promise<SSSelect>;
