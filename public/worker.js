/*
 * Lumen Web Worker (classic)
 * Loads the Emscripten-compiled WASM path tracer via importScripts.
 */

var wasmReady = false;
var pixelsPtr = 0;
var width = 0;
var height = 0;
var running = false;
var pendingRender = false;
var baseUrl = '';

self.onerror = function(e) {
  self.postMessage({
    type: 'error',
    message: 'Worker error: ' + (e.message || String(e))
  });
};

function postPixels() {
  if (!wasmReady || !pixelsPtr) return;
  var size = width * height * 4;
  // HEAPU8 is a top-level var declared by Emscripten glue, not on Module
  var raw = new Uint8ClampedArray(HEAPU8.buffer, pixelsPtr, size);
  var copy = new Uint8ClampedArray(size);
  copy.set(raw);
  self.postMessage({
    type: 'pixels', data: copy, width: width, height: height,
    samples: Module._get_total_samples(),
  });
}

function doRender(samples) {
  if (!wasmReady) return;
  running = true;
  Module._render(samples);
  postPixels();
  running = false;
  if (pendingRender) { pendingRender = false; doRender(1); }
}

// Emscripten Module — set BEFORE importScripts
self.Module = {
  locateFile: function(path) {
    return baseUrl + path;
  },
  onRuntimeInitialized: function() {
    wasmReady = true;
    // Process any pending init
    var m = self._pendingInit;
    if (m) {
      Module._init(m.width, m.height, m.sceneId);
      pixelsPtr = Module._get_pixels();
      self._pendingInit = null;
    }
    self.postMessage({ type: 'ready' });
  },
  print: function(text) { self.postMessage({ type: 'log', message: text }); },
  printErr: function(text) { self.postMessage({ type: 'error', message: text }); },
  onAbort: function(msg) {
    self.postMessage({ type: 'error', message: 'WASM abort: ' + (msg || 'unknown') });
  },
};

self.onmessage = function(e) {
  var msg = e.data;
  try {
    switch (msg.type) {
      case 'init':
        baseUrl = msg.baseUrl;
        width = msg.width;
        height = msg.height;
        if (wasmReady) {
          Module._init(msg.width, msg.height, msg.sceneId);
          pixelsPtr = Module._get_pixels();
        } else {
          // Defer until WASM is ready; load WASM now
          self._pendingInit = msg;
          try {
            self.importScripts(baseUrl + 'tracer.js');
          } catch (err) {
            self.postMessage({ type: 'error', message: 'importScripts failed: ' + (err.message || err) });
          }
        }
        break;
      case 'render':
        if (running) { pendingRender = true; }
        else doRender(msg.samples);
        break;
      case 'setCamera':
        if (!wasmReady) return;
        Module._setCamera(
          msg.eye[0], msg.eye[1], msg.eye[2],
          msg.look[0], msg.look[1], msg.look[2]
        );
        pixelsPtr = Module._get_pixels();
        break;
      case 'destroy':
        if (wasmReady) { Module._destroy(); wasmReady = false; }
        break;
    }
  } catch (err) {
    self.postMessage({ type: 'error', message: 'Handler error: ' + (err.message || err) });
  }
};
