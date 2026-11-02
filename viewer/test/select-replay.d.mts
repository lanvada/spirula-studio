// select-replay.mjs for a consumer of the release package: the shapes of
// select-fixture.json and select-fixture.expected.json, and the two calls.

export const VIEW: { readonly planar: 0; readonly packed360: 1; readonly fisheye: 2 };
export const KEPT: 0;

/** What `sam extract --motion-report` writes. */
export interface MotionReport {
  view: 'planar' | 'packed360' | 'fisheye';
  out_fov: number;
  frames: number;
  span: number;
  skip: number;
  window: number;
  max_frames: number;
  fps: number;
  adaptive_range: number;
  steps: { end: number; cost: number; drift: number; turn: number[] }[];
}

/** One clip: its motion report when adaptive, and what the decoder measured on every frame. */
export interface ReplayCase {
  name: string;
  frames: number;
  skip: number;
  keep: number;
  max_frames: number;
  span: [number, number];
  motion: MotionReport | null;
  pick: { max_blur: number; neighbour: boolean; similar_bits: number; lens: number };
  scores: number[];
  widths: number[];
  hashes: string[];
}

/** The plan, every window's pick with its verdict and blur, and the frames kept. */
export interface ReplayResult {
  name: string;
  plan: number[];
  chosen: [number, number, number][];
  kept: number[];
}

/** A synthetic texture moved by `offsets`, one Y plane an entry, `stride` frames apart. */
export interface TrackCase {
  source: [number, number];
  tracks: number;
  stride: number;
  seed: number;
  offsets: [number, number][];
  skip: number;
  keep: number;
  fps: number;
  range: number;
}

export interface TrackResult {
  view: number;
  grey: [number, number];
  weak: number;
  ends: number[];
  costs: number[];
  steps: number[][];
  plan: number[];
}

export interface Fixture {
  cases: ReplayCase[];
  track: TrackCase;
}

export interface Expected {
  cases: ReplayResult[];
  track: TrackResult;
}

/** The module from its factory and the .wasm bytes, wherever those were loaded from. */
export function instantiate<T>(
  factory: (options: {
    instantiateWasm(
      imports: WebAssembly.Imports,
      done: (instance: WebAssembly.Instance) => void,
    ): WebAssembly.Exports | Record<string, never>;
  }) => Promise<T>,
  wasm: BufferSource,
): Promise<T>;

/** `replay` and `track` over one instantiated module. */
export function conformance(M: object): {
  replay(c: ReplayCase): ReplayResult;
  track(t: TrackCase): TrackResult;
};
