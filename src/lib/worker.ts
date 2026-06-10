/*
 * Lumen Web Worker — loads the WASM path tracer and handles rendering.
 *
 * The WASM module (tracer.js) lives in /public and is loaded via
 * dynamic import at runtime, not bundled by Vite.
 */

type WorkerMessage =
  | { type: 'init'; width: number; height: number; sceneId: number }
  | { type: 'render'; samples: number }
  | { type: 'setCamera'; eye: [number,number,number]; look: [number,number,number] }
  | { type: 'destroy' };

let wasm: any = null;
let pixelsPtr = 0;
let width = 0;
let height = 0;
let running = false;
let pendingRender = false;

async function loadWasm(): Promise<void> {
  // Dynamic import from /public — Vite leaves this alone
  const mod = await import(/* @vite-ignore */ '/tracer.js');
  wasm = await mod.default();
}

function postPixels() {
  if (!wasm || !pixelsPtr) return;
  const size = width * height * 4;
  const raw = new Uint8ClampedArray(wasm.HEAPU8.buffer, pixelsPtr, size);
  const copy = new Uint8ClampedArray(size);
  copy.set(raw);
  self.postMessage(
    { type: 'pixels', data: copy, width, height, samples: wasm._get_total_samples() },
  );
}

async function doRender(samples: number) {
  if (!wasm) return;
  running = true;
  wasm._render(samples);
  postPixels();
  running = false;
  if (pendingRender) {
    pendingRender = false;
    doRender(1);
  }
}

self.onmessage = async (e: MessageEvent<WorkerMessage>) => {
  const msg = e.data;

  switch (msg.type) {
    case 'init':
      if (!wasm) await loadWasm();
      width = msg.width;
      height = msg.height;
      wasm._init(width, height, msg.sceneId);
      pixelsPtr = wasm._get_pixels();
      break;

    case 'render':
      if (running) { pendingRender = true; }
      else doRender(msg.samples);
      break;

    case 'setCamera':
      if (!wasm) return;
      wasm._setCamera(
        msg.eye[0], msg.eye[1], msg.eye[2],
        msg.look[0], msg.look[1], msg.look[2],
      );
      pixelsPtr = wasm._get_pixels();
      break;

    case 'destroy':
      if (wasm) { wasm._destroy(); wasm = null; }
      break;
  }
};
