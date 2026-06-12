import { useCallback, useEffect, useRef, useState } from 'react';
import { RenderCanvas } from './components/RenderCanvas';
import { ScenePicker } from './components/ScenePicker';
import { Controls } from './components/Controls';
import { Stats } from './components/Stats';
import { Sparkles, Code } from 'lucide-react';

const SCENES = [
  { id: 0, name: 'Cornell Box', desc: 'Classic test scene — spheres in a color-bleeding room' },
  { id: 1, name: 'Metal Spheres', desc: 'Diffuse and metal spheres under sunlight' },
  { id: 2, name: 'Glass & Light', desc: 'Dielectric glass spheres with refraction and caustics' },
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

  const workerRef = useRef<Worker | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const rafRef = useRef<number>(0);
  const samplesRef = useRef(0);
  const startTimeRef = useRef(0);
  const pendingFrameRef = useRef<ImageData | null>(null);
  const renderingRef = useRef(false);
  const sceneIdRef = useRef(0);

  const currentScene = SCENES[sceneId];

  // Create worker once on mount
  useEffect(() => {
    const baseUrl = import.meta.env.BASE_URL;
    const worker = new Worker(baseUrl + 'worker.js');

    worker.onmessage = (e) => {
      const msg = e.data;
      switch (msg.type) {
        case 'log':
          console.log('[WASM]', msg.message);
          break;
        case 'pixels': {
          const { data, width, height, samples: s } = msg;
          const img = new ImageData(data, width, height);
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
          renderingRef.current = false;
          break;
        case 'ready':
          setWasmReady(true);
          // Init the current scene once WASM is ready
          worker.postMessage({
            type: 'init',
            width: CANVAS_W, height: CANVAS_H,
            sceneId: sceneIdRef.current,
            baseUrl,
          });
          break;
      }
    };

    worker.onerror = (e) => {
      setError(e.message || 'Worker error');
      setRendering(false);
      renderingRef.current = false;
    };

    workerRef.current = worker;

    return () => {
      worker.terminate();
      workerRef.current = null;
    };
  }, []); // Only on mount/unmount

  // Handle scene changes: send new init to existing worker
  useEffect(() => {
    sceneIdRef.current = sceneId;
    const worker = workerRef.current;
    if (!worker) return;

    const baseUrl = import.meta.env.BASE_URL;
    setSamples(0);
    samplesRef.current = 0;
    setRaysPerSec(0);
    setRenderTime(0);
    setError(null);
    setRendering(false);
    renderingRef.current = false;

    worker.postMessage({
      type: 'init',
      width: CANVAS_W, height: CANVAS_H,
      sceneId,
      baseUrl,
    });
  }, [sceneId]);

  // Animation loop: draw pending frames to canvas
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
    return () => {
      running = false;
      cancelAnimationFrame(rafRef.current);
    };
  }, []);

  const handleStart = useCallback(() => {
    const worker = workerRef.current;
    if (!worker) return;
    setRendering(true);
    renderingRef.current = true;
    startTimeRef.current = performance.now();
    setRenderTime(0);
    setRaysPerSec(0);

    const pump = () => {
      if (!renderingRef.current) return;
      const w = workerRef.current;
      if (!w) return;
      w.postMessage({ type: 'render', samples: 1 });

      // Use rAF-aligned timing for smoother pump with backpressure
      requestAnimationFrame(() => {
        if (renderingRef.current) pump();
      });
    };
    pump();
  }, []);

  const handlePause = useCallback(() => {
    setRendering(false);
    renderingRef.current = false;
  }, []);

  const handleSceneChange = useCallback((id: number) => {
    setSceneId(id);
  }, []);

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
          <a
            href="https://github.com/AieatAssam/lumen"
            target="_blank"
            rel="noopener noreferrer"
            className="flex items-center gap-2 rounded-lg border border-border/50 px-3 py-1.5 text-xs text-muted-foreground transition-colors hover:border-primary/30 hover:text-foreground"
          >
            <Code className="h-3.5 w-3.5" />
            Source
          </a>
        </div>
      </header>

      {/* Main */}
      <main className="mx-auto max-w-5xl space-y-6 px-6 py-8">
        {/* Scene picker */}
        <ScenePicker
          scenes={[...SCENES]}
          activeId={sceneId}
          onChange={handleSceneChange}
          disabled={rendering}
        />

        {/* Error */}
        {error && (
          <div className="rounded-lg border border-destructive/30 bg-destructive/10 px-4 py-3 text-sm text-destructive">
            {error}
          </div>
        )}

        {/* Loading indicator */}
        {!wasmReady && (
          <div className="rounded-lg border border-primary/20 bg-primary/5 px-4 py-3 text-sm text-primary/80">
            Loading WASM engine…
          </div>
        )}

        {/* Canvas */}
        <RenderCanvas
          ref={canvasRef}
          width={CANVAS_W}
          height={CANVAS_H}
          samples={samples}
        />

        {/* Controls + Stats */}
        <div className="flex flex-wrap items-start gap-6">
          <Controls
            onStart={handleStart}
            onPause={handlePause}
            rendering={rendering}
            hasSamples={samples > 0}
          />
          <Stats
            samples={samples}
            raysPerSec={raysPerSec}
            renderTime={renderTime}
            width={CANVAS_W}
            height={CANVAS_H}
          />
        </div>

        {/* Description */}
        <div className="rounded-xl border border-border/30 bg-card/50 p-5">
          <h3 className="mb-1 text-sm font-semibold text-foreground">
            {currentScene.name}
          </h3>
          <p className="text-xs leading-relaxed text-muted-foreground">
            {currentScene.desc}
          </p>
          <p className="mt-3 text-[11px] leading-relaxed text-muted-foreground/60">
            Physically-based path tracing running entirely in your browser via
            WebAssembly. Each pixel traces rays of light as they bounce through
            the scene — diffuse scattering, specular reflections, and dielectric
            refraction with Fresnel effects. No GPU required.
          </p>
        </div>
      </main>
    </div>
  );
}
