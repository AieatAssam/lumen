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

  const workerRef = useRef<Worker | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const rafRef = useRef<number>(0);
  const samplesRef = useRef(0);
  const startTimeRef = useRef(0);
  const pendingFrameRef = useRef<ImageData | null>(null);

  const currentScene = SCENES[sceneId];

  // Initialize worker
  const initWorker = useCallback((id: number) => {
    workerRef.current?.terminate();
    setSamples(0);
    samplesRef.current = 0;
    setRaysPerSec(0);
    setRenderTime(0);
    setError(null);

    const worker = new Worker(
      new URL('./lib/worker.ts', import.meta.url),
      { type: 'module' }
    );

    worker.onmessage = (e) => {
      const msg = e.data;
      switch (msg.type) {
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
        case 'progress':
          break;
      }
    };

    worker.onerror = (e) => {
      setError(e.message || 'Worker error');
      setRendering(false);
    };

    worker.postMessage({ type: 'init', width: CANVAS_W, height: CANVAS_H, sceneId: id });
    workerRef.current = worker;
  }, []);

  // Render animation loop — draws pending frames to canvas
  useEffect(() => {
    const render = () => {
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
    return () => cancelAnimationFrame(rafRef.current);
  }, []);

  // Initialize on mount and scene change
  useEffect(() => {
    initWorker(sceneId);
    return () => workerRef.current?.terminate();
  }, [sceneId, initWorker]);

  const handleStart = useCallback(() => {
    if (!workerRef.current) return;
    setRendering(true);
    startTimeRef.current = performance.now();
    setRenderTime(0);
    setRaysPerSec(0);

    const pump = () => {
      if (!workerRef.current) return;
      // Ask worker to render 1 sample per pixel per pump
      workerRef.current.postMessage({ type: 'render', samples: 1 });
      // Queue next pump
      setTimeout(pump, 0);
    };
    pump();
  }, []);

  const handlePause = useCallback(() => {
    setRendering(false);
  }, []);

  const handleSceneChange = useCallback((id: number) => {
    setSceneId(id);
    setRendering(false);
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
            href="https://github.com"
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
