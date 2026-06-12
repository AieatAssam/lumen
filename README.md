# Lumen — WASM Path Tracer

Physically-based path tracing running entirely in your browser via WebAssembly. No GPU required.

[![GitHub Pages](https://img.shields.io/badge/demo-GitHub%20Pages-blue)](https://aieatassam.github.io/lumen/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

![Lumen Screenshot](screenshot.png)

## Features

- **Real-time progressive path tracing** — watch the image converge sample by sample
- **Three physically-based scenes**:
  - **Cornell Box** — Classic test scene with color-bleeding walls, metal sphere, and glass sphere
  - **Metal Spheres** — Diffuse and specular spheres with roughness under sunlight
  - **Glass & Light** — Dielectric glass spheres with Fresnel refraction and caustics
- **Full BRDF support** — Lambertian diffuse, metallic specular (with roughness), dielectric refraction (with Schlick Fresnel)
- **Russian roulette** path termination for efficient rendering
- **Reinhard tone mapping** with gamma correction
- **WebAssembly core** — hand-written C ray tracer compiled via Emscripten
- **Zero dependencies at runtime** — renders directly to Canvas2D

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Ray tracer core | C (550 lines) |
| WASM compiler | Emscripten 6.0 |
| Build tool | Vite |
| UI framework | React 18 + TypeScript |
| Styling | Tailwind CSS 4 |
| Deployment | GitHub Pages |

## How It Works

1. A C-based path tracer (`src/wasm/tracer.c`) implements ray-sphere intersection, BSDF sampling, and Monte Carlo integration
2. Emscripten compiles it to `tracer.wasm` (25KB) + JS glue code
3. A classic Web Worker loads the WASM module via `importScripts`
4. The React UI sends render commands to the worker; progressive frames are drawn to a Canvas2D element

## Local Development

```bash
# Install dependencies
npm install

# Start dev server
npm run dev

# Build for production
npm run build
```

The dev server runs on `http://localhost:5173/lumen/`.

## Project Structure

```
lumen/
├── public/
│   ├── worker.js        # Web Worker (classic) — loads WASM, handles rendering
│   ├── tracer.js        # Emscripten glue code
│   └── tracer.wasm      # Compiled WASM binary
├── src/
│   ├── wasm/
│   │   └── tracer.c     # C path tracer source
│   ├── components/      # React UI components
│   ├── App.tsx          # Main app component
│   └── main.tsx         # Entry point
├── index.html
├── vite.config.ts
└── package.json
```

## Building the WASM Core

Requires [Emscripten](https://emscripten.org/) installed:

```bash
emcc src/wasm/tracer.c -O3 \
  -s WASM=1 \
  -s EXPORTED_FUNCTIONS='["_init","_render","_get_pixels","_set_camera","_destroy","_get_total_samples","_malloc","_free"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -o public/tracer.js
```

## License

MIT — see [LICENSE](LICENSE) for details.
