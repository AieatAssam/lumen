/*
 * Lumen Web Worker (classic)
 * Loads the Emscripten-compiled WASM path tracer via importScripts.
 * WASM is loaded ONCE at startup; 'init' messages configure scene parameters.
 */

var wasmReady = false;
var wasmLoading = false;
var wasmFailed = false;
var pixelsPtr = 0;
var width = 0;
var height = 0;
var running = false;
var pendingRender = false;

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
    // baseUrl is set from self.location (derived from worker script URL)
    return self._baseUrl + path;
  },
  onRuntimeInitialized: function() {
    wasmReady = true;
    // Process any pending init
    var m = self._pendingInit;
    if (m) {
      width = m.width;
      height = m.height;
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

// Derive base URL from our own script location
(function() {
  var url = self.location.href;
  var idx = url.lastIndexOf('/');
  self._baseUrl = url.substring(0, idx + 1);
  // Load WASM immediately
  try {
    self.importScripts(self._baseUrl + 'tracer.js');
  } catch (err) {
    wasmFailed = true;
    self.postMessage({ type: 'error', message: 'importScripts failed: ' + (err.message || err) });
  }
})();

self.onmessage = function(e) {
  var msg = e.data;
  try {
    switch (msg.type) {
      case 'init':
        width = msg.width;
        height = msg.height;
        if (wasmReady) {
          Module._init(msg.width, msg.height, msg.sceneId);
          pixelsPtr = Module._get_pixels();
        } else {
          self._pendingInit = msg;
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
      case 'lookAt':
        if (!wasmReady) return;
        Module._look_at(msg.distance, msg.yaw, msg.pitch);
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
