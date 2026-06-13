# Lumen — WASM Path Tracer

Physically-based path tracing running entirely in your browser via WebAssembly. Diffuse, specular, and dielectric materials with Fresnel — no GPU required.

[![GitHub Pages](https://img.shields.io/badge/demo-GitHub%20Pages-blue)](https://aieatassam.github.io/lumen/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

## Features

- **6 physically-based scenes** — Cornell Box with nested refraction, outdoor metals, dielectric showcase, procedural random field, checkerboard columns, cosmic orbs
- **Nested sphere rendering** — glass shells with emissive cores create dramatic Fresnel+refraction interplay
- **Full BRDF support** — Lambertian diffuse, metallic specular (0→rough), dielectric refraction with Schlick Fresnel across multiple IORs (1.31→2.42)
- **8 CC0 HDRI environment maps** — 1024×512 equirectangular from Poly Haven, bilinear sampling
- **Progressive path tracing** — Monte Carlo integration, Russian roulette termination
- **ACES filmic tone mapping** + bloom post-processing
- **WebAssembly core** — hand-written C (1,000+ lines), compiled via Emscripten
- **Zero runtime dependencies** — renders directly to Canvas2D via worker handshake pump

## Quick Start

```bash
npm install
npm run dev      # → http://localhost:5173/lumen/
npm run build    # → dist/
```

## Building the WASM Core

Requires [Emscripten](https://emscripten.org/):

```bash
emcc src/wasm/tracer.c -O3 \
  -s WASM=1 \
  -s EXPORTED_FUNCTIONS='["_init","_render","_get_pixels","_set_camera","_look_at","_destroy","_get_total_samples","_malloc","_free","_load_env_map","_set_use_env_map","_get_env_map_active"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -o public/tracer.js
```

## Generating HDRI Environment Maps

```bash
# Download HDRs from Poly Haven into public/hdri/
# Then:
python3 hdr_to_bin.py   # Decodes RGBE RLE → 1024×512 float32 .bin → public/hdri_1024x512/
```

---

## Technical Deep Dive

### Architecture

```
┌──────────────┐    postMessage     ┌──────────────┐
│  React UI    │ ◄────────────────► │ Web Worker   │
│  (main thread)│   handshake pump  │              │
│              │                    │ importScripts│
│  Canvas2D    │ ◄── pixels ─────── │ tracer.js    │
│  bloom pass  │                    │   + wasm     │
└──────────────┘                    └──────────────┘
```

The render loop uses a **handshake pattern** to prevent message backlog. The main thread sends `{type:'render', samples:N}`, the worker processes one batch, posts back `{type:'pixels', data}` followed by `{type:'done'}`, and the main thread sends the next batch only after receiving `done`. This means pause is instantaneous — just stop sending.

### Path Tracing Algorithm

Lumen implements **unbiased Monte Carlo path tracing**:

1. **Ray generation** — For each pixel, jittered samples are generated within the pixel footprint. A ray is cast from the camera through the sample point.

2. **Ray-sphere intersection** — The scene is a collection of spheres (including giant ones for walls/floors). The intersection test solves the quadratic `|O + tD - C|² = r²`. The nearest hit with `t > 0.001` is kept.

3. **BSDF sampling** — At each hit point, the material determines how the ray continues:

   | Material | BRDF | Sampling |
   |----------|------|----------|
   | **Lambertian** (diffuse) | `albedo / π` | Cosine-weighted hemisphere |
   | **Metal** (specular) | Perfect reflection | Mirror direction + roughness blur |
   | **Dielectric** (glass) | Refraction + Fresnel reflection | Schlick approximation, Snell's law |
   | **Emissive** (light) | Returns emission | Terminates path |

4. **Russian roulette** — After 3 bounces, paths are probabilistically terminated with probability proportional to the material albedo. Surviving paths are weighted up to remain unbiased.

5. **Accumulation** — Each sample's radiance is averaged into a running mean: `accum = (old * N + new) / (N + 1)`. This converges toward the true integral as samples increase.

### Nested Spheres

A unique feature: placing a small emissive or colored sphere at the **exact same center** as a larger glass sphere. When a ray hits the glass shell, it refracts inward, strikes the inner core (which may glow), then refracts back out through the glass. This creates:

- **Fresnel reflection** on the outer glass surface (environment reflections)
- **Visible inner glow** through the glass, tinted by the glass color
- **Refraction distortion** — the inner core appears warped by the glass curvature

The ray tracer handles this naturally — no special case needed. The intersection routine simply finds the nearest hit at each step.

### Environment Map Sampling

HDRI environment maps are stored as 1024×512 float32 RGB arrays (equirectangular projection). When a ray misses all scene geometry, the background radiance is computed via bilinear interpolation:

```
phi   = atan2(dir.z, dir.x)     // azimuth [-π, π]
theta = asin(dir.y)             // elevation [-π/2, π/2]
u     = 0.5 + phi / (2π)        // horizontal [0, 1]
v     = 0.5 - theta / π         // vertical [0, 1], flipped so v=0 = zenith
```

HDRIs are pre-normalized with percentile clamping to prevent ACES bleaching:
- 99.9th percentile → scaled to 4.0
- Hard clamp at `max(1.2 × 99.99th, 12.0)`
- C-code safety clamp: all samples capped at [0, 15]

### ACES Filmic Tone Mapping

The ACES (Academy Color Encoding System) approximation maps HDR radiance to SDR display:

```
f(x) = x * (2.51x + 0.03) / (x * (2.43x + 0.59) + 0.14)
```

Unlike Reinhard, ACES preserves color saturation in highlights and produces a natural "filmic" roll-off with a gentle shoulder.

### Bloom Post-Processing

After tone mapping, a bloom pass adds glow to bright regions:
1. **Threshold** — extract pixels where luminance > 0.70
2. **Box blur** — two-pass Gaussian approximation at 6px and 14px radii
3. **Composite** — `lighter` blend at 0.30 alpha over the original

### WASM → Worker Communication

The C API exposed to JavaScript:

| Function | Purpose |
|----------|---------|
| `init(w, h, sceneId)` | Allocate buffers, set up scene geometry |
| `render(spp)` | Trace `spp` samples per pixel, accumulate, tone-map |
| `get_pixels()` | Returns pointer to RGBA8 output buffer |
| `load_env_map(data, w, h)` | Copy float32 HDRI into WASM heap |
| `set_use_env_map(bool)` | Toggle procedural sky / HDRI background |
| `set_camera(ex,ey,ez, lx,ly,lz)` | Reposition camera, reset accumulation |
| `look_at(dist, yaw, pitch)` | Orbit camera around look-at point |

### File Structure

```
lumen/
├── public/
│   ├── worker.js              # Classic Web Worker — handshake render pump
│   ├── tracer.js / tracer.wasm # Emscripten output
│   └── hdri_1024x512/         # 8 × 6.3MB float32 HDRI binaries
├── src/
│   ├── wasm/tracer.c          # C path tracer (~1,100 lines)
│   ├── components/            # ScenePicker, Controls, Stats, RenderCanvas
│   ├── App.tsx                # Main app — worker lifecycle, bloom, env map loader
│   └── main.tsx               # ReactDOM entry
├── tests/e2e.spec.mjs         # 23 Playwright E2E tests
├── hdr_to_bin.py              # RGBE RLE decoder → float32 converter
└── .github/workflows/deploy.yml
```

## HDRI Environment Maps

All HDRIs from [Poly Haven](https://polyhaven.com), CC0 licensed (no rights reserved).

| Map | Source |
|-----|--------|
| ☀️ Clear Sky | [Kloofendal Partly Cloudy](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) |
| 🌙 Starry Night | [Rogland Clear Night](https://polyhaven.com/a/rogland_clear_night) |
| 🌅 Venice Sunset | [Venice Sunset](https://polyhaven.com/a/venice_sunset) |
| 🏠 Studio Hall | [Studio Apartment](https://polyhaven.com/a/studio_small_07) |
| 🏭 Industrial | [Industrial Sunset](https://polyhaven.com/a/industrial_sunset) |
| ❄️ Snow Field | [Snowy Field](https://polyhaven.com/a/snowy_field) |
| 🌥️ Overcast | [Overcast Sky](https://polyhaven.com/a/overcast) |
| 🛣️ Evening Road | [Sunset Field](https://polyhaven.com/a/sunset_field) |

## License

MIT — see [LICENSE](LICENSE) for details.
