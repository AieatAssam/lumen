import { useCallback, useEffect, useRef, useState } from 'react';
import { ScenePicker } from './components/ScenePicker';
import { Controls } from './components/Controls';
import { Stats } from './components/Stats';
import { Sparkles, Code, RotateCw, ZoomIn, ZoomOut, ArrowUp, ArrowDown, ArrowLeft, ArrowRight } from 'lucide-react';

const SCENES = [
  { id: 0, name: 'Cornell Box', desc: 'Sapphire, emerald, amber, diamond, copper & brushed metal — all three material types on display' },
  { id: 1, name: 'Metal Spheres', desc: 'Mirror → brushed → rough metal plus ruby & aqua glass under procedural sky' },
  { id: 2, name: 'Glass & Light', desc: 'Five dielectrics (glass, diamond, crystal, amber, amethyst) with metal accents' },
  { id: 3, name: 'Random Spheres', desc: 'Procedural field: 50/50 metal/dielectric split with a center diamond' },
  { id: 4, name: 'Checkerboard', desc: 'Metal & glass columns, row of gemstones (ruby/diamond/emerald/sapphire)' },
  { id: 5, name: 'Cosmic', desc: '200 stars, 10 nebula clouds, 8 floating orbs — ice, coral, emerald, diamond, gold' },
] as const;

const ENV_MAPS = [
  { id: -1, name: '☁️ Procedural', file: null, desc: 'Generated sky with clouds' },
  { id: 0, name: '☀️ Clear Sky', file: 'kloofendal_1k.bin', desc: 'Midday partly cloudy sky — Poly Haven (CC0)' },
  { id: 1, name: '🌙 Starry Night', file: 'rogland_night_1k.bin', desc: 'Milky Way night sky — Poly Haven (CC0)' },
  { id: 2, name: '🌅 Venice Sunset', file: 'venice_sunset_1k.bin', desc: 'Golden hour over water — Poly Haven (CC0)' },
  { id: 3, name: '🏠 Studio Hall', file: 'studio_1k.bin', desc: 'Indoor hall with chandeliers — Poly Haven (CC0)' },
  { id: 4, name: '🏭 Industrial', file: 'industrial_sunset_1k.bin', desc: 'Sunset behind factories — Poly Haven (CC0)' },
  { id: 5, name: '❄️ Snow Field', file: 'snowy_field_1k.bin', desc: 'Snowy landscape under overcast sky — Poly Haven (CC0)' },
  { id: 6, name: '🌥️ Overcast', file: 'overcast_1k.bin', desc: 'Cloudy sky over green hills — Poly Haven (CC0)' },
  { id: 7, name: '🛣️ Evening Road', file: 'sunset_field_1k.bin', desc: 'Country road at dusk — Poly Haven (CC0)' },
] as const;

const CANVAS_W = 640;
const CANVAS_H = 360;

// ── Bloom post-process ──
// Extracts bright pixels, applies box blur, blends back for a glow effect
function applyBloom(src: ImageData, dst: CanvasRenderingContext2D, bloomCanvas: HTMLCanvasElement) {
  const w = src.width;
  const h = src.height;
  bloomCanvas.width = w;
  bloomCanvas.height = h;
  const bctx = bloomCanvas.getContext('2d')!;

  // 1. Extract bright pixels (threshold > 0.75 for less bloom)
  const bright = bctx.createImageData(w, h);
  const srcData = src.data;
  const brightData = bright.data;
  for (let i = 0; i < srcData.length; i += 4) {
    const r = srcData[i] / 255;
    const g = srcData[i + 1] / 255;
    const b = srcData[i + 2] / 255;
    const lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const t = lum > 0.70 ? (lum - 0.70) / 0.30 : 0; // higher threshold
    brightData[i] = srcData[i] * t;
    brightData[i + 1] = srcData[i + 1] * t;
    brightData[i + 2] = srcData[i + 2] * t;
    brightData[i + 3] = 255;
  }
  bctx.putImageData(bright, 0, 0);

  // 2. Apply 2-pass blur at smaller radii
  bctx.globalCompositeOperation = 'source-over';
  bctx.filter = 'blur(6px)';
  bctx.drawImage(bloomCanvas, 0, 0);
  bctx.filter = 'blur(14px)';
  bctx.drawImage(bloomCanvas, 0, 0);
  bctx.filter = 'none';

  // 3. Composite bloom over original (lighter blend, reduced strength)
  dst.putImageData(src, 0, 0);
  dst.globalCompositeOperation = 'lighter';
  dst.globalAlpha = 0.30;
  dst.drawImage(bloomCanvas, 0, 0);
  dst.globalCompositeOperation = 'source-over';
  dst.globalAlpha = 1.0;
}

export default function App() {
  const [sceneId, setSceneId] = useState(0);
  const [samples, setSamples] = useState(0);
  const [rendering, setRendering] = useState(false);
  const [raysPerSec, setRaysPerSec] = useState(0);
  const [renderTime, setRenderTime] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [wasmReady, setWasmReady] = useState(false);
  const [envMapId, setEnvMapId] = useState(-1);  // -1 = procedural sky
  const [envMapLoading, setEnvMapLoading] = useState(false);

  // Camera orbit state
  const [distance, setDistance] = useState(5.0);
  const [yaw, setYaw] = useState(0);
  const [pitch, setPitch] = useState(15);

  const workerRef = useRef<Worker | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const bloomRef = useRef<HTMLCanvasElement | null>(null);
  const rafRef = useRef<number>(0);
  const samplesRef = useRef(0);
  const startTimeRef = useRef(0);
  const pendingFrameRef = useRef<ImageData | null>(null);
  const activeRef = useRef(false);
  const wantsRenderRef = useRef(false);
  const sceneIdRef = useRef(0);
  const distanceRef = useRef(5.0);
  const yawRef = useRef(0);
  const pitchRef = useRef(15);

  const currentScene = SCENES[sceneId];

  // ── Create worker once on mount ──
  useEffect(() => {
    const baseUrl = import.meta.env.BASE_URL;
    const worker = new Worker(baseUrl + 'worker.js');

    worker.onmessage = (e) => {
      const msg = e.data;
      switch (msg.type) {
        case 'log':
          break;
        case 'pixels': {
          const { data, width, height, samples: s } = msg;
          const img = new ImageData(new Uint8ClampedArray(data), width, height);
          pendingFrameRef.current = img;
          samplesRef.current = s;
          setSamples(s);
          const elapsed = (performance.now() - startTimeRef.current) / 1000;
          if (elapsed > 0.1) {
            setRaysPerSec(Math.round((s * width * height) / elapsed));
            setRenderTime(elapsed);
          }
          break;
        }
        case 'error':
          setError(msg.message);
          setRendering(false);
          activeRef.current = false;
          wantsRenderRef.current = false;
          break;
        case 'ready':
          setWasmReady(true);
          break;
        case 'done':
          if (wantsRenderRef.current) {
            const w = workerRef.current;
            if (w) w.postMessage({ type: 'render', samples: 1 });
          }
          break;
      }
    };

    worker.onerror = (e) => {
      setError(e.message || 'Worker error');
      setRendering(false);
      activeRef.current = false;
      wantsRenderRef.current = false;
    };

    workerRef.current = worker;

    worker.postMessage({
      type: 'init', width: CANVAS_W, height: CANVAS_H,
      sceneId: sceneIdRef.current, baseUrl,
    });

    return () => { worker.terminate(); workerRef.current = null; };
  }, []);

  // ── Scene change ──
  useEffect(() => {
    sceneIdRef.current = sceneId;
    const worker = workerRef.current;
    if (!worker) return;
    activeRef.current = false;
    wantsRenderRef.current = false;
    setRendering(false);
    setSamples(0); samplesRef.current = 0;
    setRaysPerSec(0); setRenderTime(0);
    setError(null);
    worker.postMessage({
      type: 'init', width: CANVAS_W, height: CANVAS_H,
      sceneId, baseUrl: import.meta.env.BASE_URL,
    });
  }, [sceneId]);

  useEffect(() => {
    distanceRef.current = distance;
    yawRef.current = yaw;
    pitchRef.current = pitch;
  }, [distance, yaw, pitch]);

  const applyLookAt = useCallback((dist: number, y: number, p: number) => {
    const w = workerRef.current;
    if (!w) return;
    w.postMessage({ type: 'lookAt', distance: dist, yaw: y, pitch: p });
  }, []);

  const doCamera = useCallback((fn: (d: number, y: number, p: number) => [number, number, number]) => {
    const newDist = distanceRef.current;
    const newYaw = yawRef.current;
    const newPitch = pitchRef.current;
    const [d, y, p] = fn(newDist, newYaw, newPitch);
    setDistance(d); setYaw(y); setPitch(p);
    distanceRef.current = d; yawRef.current = y; pitchRef.current = p;
    applyLookAt(d, y, p);
  }, [applyLookAt]);

  // ── Animation loop with bloom ──
  useEffect(() => {
    let running = true;
    const render = () => {
      if (!running) return;
      rafRef.current = requestAnimationFrame(render);
      const canvas = canvasRef.current;
      const pending = pendingFrameRef.current;
      if (!canvas || !pending) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      canvas.width = pending.width;
      canvas.height = pending.height;

      // Apply bloom if bloom canvas exists
      const bloomCanvas = bloomRef.current;
      if (bloomCanvas) {
        applyBloom(pending, ctx, bloomCanvas);
      } else {
        ctx.putImageData(pending, 0, 0);
      }
      pendingFrameRef.current = null;
    };
    rafRef.current = requestAnimationFrame(render);
    return () => { running = false; cancelAnimationFrame(rafRef.current); };
  }, []);

  // ── Render pump (handshake pattern) ──
  const startPump = useCallback(() => {
    setRendering(true);
    activeRef.current = true;
    wantsRenderRef.current = true;
    startTimeRef.current = performance.now();
    setRenderTime(0); setRaysPerSec(0);
    const w = workerRef.current;
    if (w) w.postMessage({ type: 'render', samples: 1 });
  }, []);

  const stopPump = useCallback(() => {
    setRendering(false);
    activeRef.current = false;
    wantsRenderRef.current = false;
  }, []);

  const handleStart = useCallback(() => {
    if (!workerRef.current) return;
    startPump();
  }, [startPump]);

  const handlePause = useCallback(() => stopPump(), [stopPump]);
  const handleSceneChange = useCallback((id: number) => setSceneId(id), []);
  const handleEnvMapChange = useCallback((id: number) => {
    setEnvMapId(id);
    const worker = workerRef.current;
    if (!worker) return;
    if (id < 0) {
      // Procedural sky
      worker.postMessage({ type: 'setUseEnvMap', use: false });
      return;
    }
    const map = ENV_MAPS.find(m => m.id === id);
    if (!map || !map.file) return;
    setEnvMapLoading(true);
    const baseUrl = import.meta.env.BASE_URL;
    fetch(baseUrl + 'hdri_128x64/' + map.file)
      .then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.arrayBuffer();
      })
      .then(buf => {
        const floats = new Float32Array(buf);
        worker.postMessage({
          type: 'loadEnvMap',
          data: floats,
          width: 128,
          height: 64,
        }, [floats.buffer]); // transfer ownership for speed
        worker.postMessage({ type: 'setUseEnvMap', use: true });
        setEnvMapLoading(false);
      })
      .catch(err => {
        setError('Failed to load env map: ' + err.message);
        setEnvMapLoading(false);
        setEnvMapId(-1);
      });
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      switch (e.key) {
        case 'ArrowLeft':  doCamera((d, y, p) => [d, y - 5, p]); break;
        case 'ArrowRight': doCamera((d, y, p) => [d, y + 5, p]); break;
        case 'ArrowUp':    doCamera((d, y, p) => [d, y, Math.min(89, p + 5)]); break;
        case 'ArrowDown':  doCamera((d, y, p) => [d, y, Math.max(-89, p - 5)]); break;
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [doCamera]);

  return (
    <div className="min-h-screen bg-background text-foreground">
      {/* Header */}
      <header className="border-b border-border/50 bg-card/30 backdrop-blur-sm">
        <div className="mx-auto flex max-w-5xl items-center justify-between px-3 sm:px-6 py-3 sm:py-4">
          <div className="flex items-center gap-2 sm:gap-3">
            <div className="flex h-7 w-7 sm:h-9 sm:w-9 items-center justify-center rounded-lg bg-primary/20">
              <Sparkles className="h-4 w-4 sm:h-5 sm:w-5 text-primary" />
            </div>
            <div>
              <h1 className="text-base sm:text-xl font-bold tracking-tight">Lumen</h1>
              <p className="text-[10px] sm:text-xs text-muted-foreground">WASM Path Tracer</p>
            </div>
          </div>
          <a href="https://github.com/AieatAssam/lumen" target="_blank" rel="noopener noreferrer"
            className="flex items-center gap-1.5 rounded-lg border border-border/50 px-2 sm:px-3 py-1 sm:py-1.5 text-[10px] sm:text-xs text-muted-foreground transition-colors hover:border-primary/30 hover:text-foreground">
            <Code className="h-3 w-3 sm:h-3.5 sm:w-3.5" />Source
          </a>
        </div>
      </header>

      <main className="mx-auto max-w-5xl space-y-4 sm:space-y-6 px-3 sm:px-6 py-4 sm:py-8">
        {/* Scene picker — horizontal scroll on mobile */}
        <ScenePicker scenes={[...SCENES]} activeId={sceneId} onChange={handleSceneChange} disabled={rendering} />

        {/* Env map picker */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-3 sm:p-4">
          <h3 className="mb-2 sm:mb-3 text-xs sm:text-sm font-semibold text-foreground">
            🌍 Background Environment
            {envMapLoading && <span className="ml-2 text-primary/60 animate-pulse">Loading…</span>}
          </h3>
          <div className="flex flex-wrap gap-1.5 sm:gap-2">
            {ENV_MAPS.map(env => (
              <button
                key={env.id}
                onClick={() => handleEnvMapChange(env.id)}
                disabled={rendering || envMapLoading}
                className={`rounded-lg border px-2.5 sm:px-3 py-1.5 sm:py-2 text-[10px] sm:text-xs font-medium transition-all min-h-[36px]
                  ${envMapId === env.id
                    ? 'border-primary bg-primary/10 text-primary shadow-sm shadow-primary/20'
                    : 'border-border/50 bg-card/50 text-muted-foreground hover:border-primary/30 hover:text-foreground'
                  } disabled:opacity-50 disabled:cursor-not-allowed`}
                title={env.desc}
              >
                {env.name}
              </button>
            ))}
          </div>
          <p className="mt-2 text-[10px] text-muted-foreground/60">
            All HDRIs from <a href="https://polyhaven.com" target="_blank" rel="noopener noreferrer" className="underline">Poly Haven</a> (CC0 — no rights reserved)
          </p>
        </div>

        {error && (
          <div className="rounded-lg border border-destructive/30 bg-destructive/10 px-3 sm:px-4 py-2 sm:py-3 text-xs sm:text-sm text-destructive">{error}</div>
        )}

        {!wasmReady && (
          <div className="rounded-lg border border-primary/20 bg-primary/5 px-3 sm:px-4 py-2 sm:py-3 text-xs sm:text-sm text-primary/80">Loading WASM engine…</div>
        )}

        {/* Canvas — full width, scales on mobile */}
        <div className="rounded-xl overflow-hidden border border-border/30 bg-black shadow-lg">
          <canvas
            ref={canvasRef}
            className="block w-full h-auto"
            style={{ imageRendering: 'auto' }}
          />
          {/* Hidden canvas for bloom computation */}
          <canvas ref={bloomRef} className="hidden" />
        </div>

        {/* Controls + Stats — stack vertically on mobile */}
        <div className="flex flex-col sm:flex-row sm:items-start gap-3 sm:gap-6">
          <Controls onStart={handleStart} onPause={handlePause} rendering={rendering} hasSamples={samples > 0} />
          <Stats samples={samples} raysPerSec={raysPerSec} renderTime={renderTime} width={CANVAS_W} height={CANVAS_H} />
        </div>

        {/* Camera controls */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-3 sm:p-4">
          <h3 className="mb-2 sm:mb-3 text-xs sm:text-sm font-semibold text-foreground">
            Camera — use arrow keys or buttons
          </h3>
          <div className="flex flex-wrap items-center gap-1.5 sm:gap-2">
            <button onClick={() => doCamera((d, y, p) => [Math.max(1, d - 1), y, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Zoom in">
              <ZoomIn className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d + 1, y, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Zoom out">
              <ZoomOut className="h-4 w-4" />
            </button>
            <span className="text-[10px] sm:text-xs text-muted-foreground px-1 sm:px-2">Dist: {distance.toFixed(0)}</span>

            <div className="hidden sm:block w-px h-6 bg-border/50 mx-1" />

            <button onClick={() => doCamera((d, y, p) => [d, y - 15, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Pan left">
              <ArrowLeft className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y + 15, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Pan right">
              <ArrowRight className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y, Math.min(89, p + 10)])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Tilt up">
              <ArrowUp className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y, Math.max(-89, p - 10)])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 sm:p-2 text-muted-foreground hover:text-foreground min-w-[36px] min-h-[36px]" title="Tilt down">
              <ArrowDown className="h-4 w-4" />
            </button>
            <span className="text-[10px] sm:text-xs text-muted-foreground px-1 sm:px-2">Yaw: {yaw}° Pitch: {pitch}°</span>

            <button onClick={() => doCamera((d, y, p) => [5, 0, 15])}
              className="ml-auto rounded-lg border border-border/50 bg-card/50 px-2 sm:px-3 py-2 text-[10px] sm:text-xs text-muted-foreground hover:text-foreground min-h-[36px]">
              <RotateCw className="h-3 w-3 sm:h-3.5 sm:w-3.5 inline mr-1" />Reset view
            </button>
          </div>
        </div>

        {/* Description */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-3 sm:p-5">
          <h3 className="mb-1 text-xs sm:text-sm font-semibold text-foreground">{currentScene.name}</h3>
          <p className="text-[11px] sm:text-xs leading-relaxed text-muted-foreground">{currentScene.desc}</p>
          <p className="mt-2 sm:mt-3 text-[10px] sm:text-[11px] leading-relaxed text-muted-foreground/60">
            Physically-based path tracing with ACES tone mapping and bloom — entirely in your browser via WebAssembly.
            Diffuse scattering, specular reflections, and dielectric refraction with Fresnel effects. No GPU required.
          </p>
        </div>
      </main>
    </div>
  );
}
