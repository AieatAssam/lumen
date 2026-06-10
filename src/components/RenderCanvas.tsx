import { forwardRef } from 'react';

interface RenderCanvasProps {
  width: number;
  height: number;
  samples: number;
}

export const RenderCanvas = forwardRef<HTMLCanvasElement, RenderCanvasProps>(
  function RenderCanvas({ width, height, samples }, ref) {
    return (
      <div className="group relative overflow-hidden rounded-xl border border-border/30 bg-black canvas-glow">
        <canvas
          ref={ref}
          width={width}
          height={height}
          className="block w-full"
          style={{ imageRendering: 'pixelated', aspectRatio: `${width}/${height}` }}
        />
        {/* Samples badge */}
        <div className="absolute right-3 top-3 rounded-full bg-background/70 px-3 py-1 text-xs font-medium text-foreground/80 backdrop-blur-sm">
          {samples > 0
            ? `${samples} spp`
            : 'Ready'}
        </div>
        {/* Empty state */}
        {samples === 0 && (
          <div className="absolute inset-0 flex flex-col items-center justify-center gap-2 text-muted-foreground/50">
            <div className="h-12 w-12 animate-pulse rounded-full bg-primary/10" />
            <span className="text-sm font-medium">Hit Render to start</span>
          </div>
        )}
      </div>
    );
  }
);
