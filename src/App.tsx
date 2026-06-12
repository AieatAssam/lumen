import { useCallback, useEffect, useRef, useState } from 'react';
import { RenderCanvas } from './components/RenderCanvas';
import { ScenePicker } from './components/ScenePicker';
import { Controls } from './components/Controls';
import { Stats } from './components/Stats';
import { Sparkles, Code, RotateCw, ZoomIn, ZoomOut, ArrowUp, ArrowDown, ArrowLeft, ArrowRight } from 'lucide-react';

const SCENES = [
  { id: 0, name: 'Cornell Box', desc: 'Classic test scene — diffuse walls, metal, and glass' },
  { id: 1, name: 'Metal Spheres', desc: 'Specular and rough metal spheres under sunlight' },
  { id: 2, name: 'Glass & Light', desc: 'Dielectric glass with Fresnel refraction and caustics' },
  { id: 3, name: 'Random Spheres', desc: 'Procedural field of colored spheres with a glass centerpiece' },
  { id: 4, name: 'Checkerboard', desc: 'Patterned floor with metallic columns and gold sphere' },
  { id: 5, name: 'Cosmic', desc: 'Abstract floating orbs illuminated by colored lights' },
] as const;

const CANVAS_W = 640;
const CANVAS_H = 360;

export default function App() {
  const [sceneId, setSceneId] = useState(0);
  const [samples, setSamples] = useState(0);
  const [rendering, setRendering] = useState(false);
  const [raysPerSec, setRaysPerSec] = useState(0);
  const [renderTime, setRenderTime] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [wasmReady, setWasmReady] = useState(false);

  // Camera orbit state
  const [distance, setDistance] = useState(5.0);
  const [yaw, setYaw] = useState(0);
  const [pitch, setPitch] = useState(15);

  const workerRef = useRef<Worker | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const rafRef = useRef<number>(0);
  const samplesRef = useRef(0);
  const startTimeRef = useRef(0);
  const pendingFrameRef = useRef<ImageData | null>(null);
  const activeRef = useRef(false);       // true while pump is running
  const wantsRenderRef = useRef(false);   // set by startPump, cleared by stopPump
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
          // Worker finished a render frame — send next if still active
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

    // Send init immediately
    worker.postMessage({
      type: 'init', width: CANVAS_W, height: CANVAS_H,
      sceneId: sceneIdRef.current, baseUrl,
    });

    return () => { worker.terminate(); workerRef.current = null; };
  }, []);

  // ── Scene change: re-init worker ──
  useEffect(() => {
    sceneIdRef.current = sceneId;
    const worker = workerRef.current;
    if (!worker) return;
    // Stop any running render pump before re-init
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

  // ── Camera orbit changes ──
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
    setDistance(d);
    setYaw(y);
    setPitch(p);
    distanceRef.current = d;
    yawRef.current = y;
    pitchRef.current = p;
    applyLookAt(d, y, p);
  }, [applyLookAt]);

  // ── Animation loop ──
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
      ctx.putImageData(pending, 0, 0);
      pendingFrameRef.current = null;
    };
    rafRef.current = requestAnimationFrame(render);
    return () => { running = false; cancelAnimationFrame(rafRef.current); };
  }, []);

  // ── Render pump (handshake pattern — no backlog) ──
  const startPump = useCallback(() => {
    setRendering(true);
    activeRef.current = true;
    wantsRenderRef.current = true;
    startTimeRef.current = performance.now();
    setRenderTime(0); setRaysPerSec(0);

    // Send first render frame — subsequent frames are triggered by 'done' callback
    const w = workerRef.current;
    if (w) w.postMessage({ type: 'render', samples: 1 });
  }, []);

  const stopPump = useCallback(() => {
    setRendering(false);
    activeRef.current = false;
    wantsRenderRef.current = false;
  }, []);

  const handleStart = useCallback(() => {
    const w = workerRef.current;
    if (!w) return;
    startPump();
  }, [startPump]);

  const handlePause = useCallback(() => stopPump(), [stopPump]);

  const handleSceneChange = useCallback((id: number) => setSceneId(id), []);

  // ── Keyboard camera controls ──
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
        <div className="mx-auto flex max-w-5xl items-center justify-between px-6 py-4">
          <div className="flex items-center gap-3">
            <div className="flex h-9 w-9 items-center justify-center rounded-lg bg-primary/20">
              <Sparkles className="h-5 w-5 text-primary" />
            </div>
            <div>
              <h1 className="text-xl font-bold tracking-tight">Lumen</h1>
              <p className="text-xs text-muted-foreground">WASM Path Tracer</p>
            </div>
          </div>
          <a href="https://github.com/AieatAssam/lumen" target="_blank" rel="noopener noreferrer"
            className="flex items-center gap-2 rounded-lg border border-border/50 px-3 py-1.5 text-xs text-muted-foreground transition-colors hover:border-primary/30 hover:text-foreground">
            <Code className="h-3.5 w-3.5" />Source
          </a>
        </div>
      </header>

      <main className="mx-auto max-w-5xl space-y-6 px-6 py-8">
        {/* Scene picker */}
        <ScenePicker scenes={[...SCENES]} activeId={sceneId} onChange={handleSceneChange} disabled={rendering} />

        {/* Error */}
        {error && (
          <div className="rounded-lg border border-destructive/30 bg-destructive/10 px-4 py-3 text-sm text-destructive">{error}</div>
        )}

        {/* Loading */}
        {!wasmReady && (
          <div className="rounded-lg border border-primary/20 bg-primary/5 px-4 py-3 text-sm text-primary/80">Loading WASM engine…</div>
        )}

        {/* Canvas */}
        <RenderCanvas ref={canvasRef} width={CANVAS_W} height={CANVAS_H} samples={samples} />

        {/* Controls + Stats */}
        <div className="flex flex-wrap items-start gap-6">
          <Controls onStart={handleStart} onPause={handlePause} rendering={rendering} hasSamples={samples > 0} />
          <Stats samples={samples} raysPerSec={raysPerSec} renderTime={renderTime} width={CANVAS_W} height={CANVAS_H} />
        </div>

        {/* Camera controls */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-4">
          <h3 className="mb-3 text-sm font-semibold text-foreground">Camera — use arrow keys or drag</h3>
          <div className="flex flex-wrap items-center gap-2">
            <button onClick={() => doCamera((d, y, p) => [Math.max(1, d - 1), y, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Zoom in">
              <ZoomIn className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d + 1, y, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Zoom out">
              <ZoomOut className="h-4 w-4" />
            </button>
            <span className="text-xs text-muted-foreground px-2">Dist: {distance.toFixed(0)}</span>

            <div className="w-px h-6 bg-border/50 mx-1" />

            <button onClick={() => doCamera((d, y, p) => [d, y - 15, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Pan left">
              <ArrowLeft className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y + 15, p])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Pan right">
              <ArrowRight className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y, Math.min(89, p + 10)])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Tilt up">
              <ArrowUp className="h-4 w-4" />
            </button>
            <button onClick={() => doCamera((d, y, p) => [d, y, Math.max(-89, p - 10)])}
              className="rounded-lg border border-border/50 bg-card/50 p-2 text-muted-foreground hover:text-foreground" title="Tilt down">
              <ArrowDown className="h-4 w-4" />
            </button>
            <span className="text-xs text-muted-foreground px-2">Yaw: {yaw}° Pitch: {pitch}°</span>

            <button onClick={() => doCamera((d, y, p) => [5, 0, 15])}
              className="ml-auto rounded-lg border border-border/50 bg-card/50 px-3 py-2 text-xs text-muted-foreground hover:text-foreground">
              <RotateCw className="h-3.5 w-3.5 inline mr-1" />Reset view
            </button>
          </div>
        </div>

        {/* Description */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-5">
          <h3 className="mb-1 text-sm font-semibold text-foreground">{currentScene.name}</h3>
          <p className="text-xs leading-relaxed text-muted-foreground">{currentScene.desc}</p>
          <p className="mt-3 text-[11px] leading-relaxed text-muted-foreground/60">
            Physically-based path tracing running entirely in your browser via WebAssembly.
            Each pixel traces rays of light as they bounce through the scene — diffuse scattering,
            specular reflections, and dielectric refraction with Fresnel effects. No GPU required.
          </p>
        </div>
      </main>
    </div>
  );
}
