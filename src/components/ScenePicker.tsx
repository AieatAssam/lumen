import { cn } from '@/lib/utils';

interface Scene {
  id: number;
  name: string;
  desc: string;
}

interface ScenePickerProps {
  scenes: Scene[];
  activeId: number;
  onChange: (id: number) => void;
  disabled?: boolean;
}

export function ScenePicker({ scenes, activeId, onChange, disabled }: ScenePickerProps) {
  return (
    <div className="flex gap-2">
      {scenes.map((scene) => (
        <button
          key={scene.id}
          onClick={() => onChange(scene.id)}
          disabled={disabled}
          className={cn(
            'rounded-lg border px-4 py-2.5 text-sm font-medium transition-all',
            scene.id === activeId
              ? 'border-primary/50 bg-primary/10 text-primary shadow-[0_0_15px_-3px_rgba(167,139,250,0.3)]'
              : 'border-border/30 bg-card/30 text-muted-foreground hover:border-border/60 hover:text-foreground',
            disabled && 'cursor-not-allowed opacity-50'
          )}
        >
          {scene.name}
        </button>
      ))}
    </div>
  );
}
