import { Play, Pause, RotateCcw } from 'lucide-react';

interface ControlsProps {
  onStart: () => void;
  onPause: () => void;
  rendering: boolean;
  hasSamples: boolean;
}

export function Controls({ onStart, onPause, rendering, hasSamples }: ControlsProps) {
  return (
    <div className="flex items-center gap-3">
      {rendering ? (
        <button
          onClick={onPause}
          className="inline-flex items-center gap-2 rounded-lg border border-border/50 bg-card/50 px-5 py-2.5 text-sm font-medium text-foreground transition-all hover:border-primary/30 hover:bg-card"
        >
          <Pause className="h-4 w-4" />
          Pause
        </button>
      ) : (
        <button
          onClick={onStart}
          className="inline-flex items-center gap-2 rounded-lg bg-primary px-5 py-2.5 text-sm font-medium text-primary-foreground transition-all hover:bg-primary/90 shadow-[0_0_20px_-5px_rgba(167,139,250,0.4)]"
        >
          <Play className="h-4 w-4" />
          Render
        </button>
      )}

      {rendering && (
        <div className="flex items-center gap-2 text-xs text-muted-foreground">
          <div className="h-2 w-2 animate-pulse rounded-full bg-primary" />
          Rendering&hellip;
        </div>
      )}
    </div>
  );
}
