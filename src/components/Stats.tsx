import { Zap, Clock, Hash } from 'lucide-react';

interface StatsProps {
  samples: number;
  raysPerSec: number;
  renderTime: number;
  width: number;
  height: number;
}

function formatNum(n: number): string {
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'K';
  return n.toString();
}

export function Stats({ samples, raysPerSec, renderTime, width, height }: StatsProps) {
  const totalRays = samples * width * height;

  return (
    <div className="flex flex-wrap gap-4">
      <StatItem
        icon={<Hash className="h-3.5 w-3.5" />}
        label="Samples"
        value={samples > 0 ? `${samples} spp` : '—'}
      />
      <StatItem
        icon={<Zap className="h-3.5 w-3.5" />}
        label="Rays/sec"
        value={raysPerSec > 0 ? formatNum(raysPerSec) : '—'}
      />
      <StatItem
        icon={<Clock className="h-3.5 w-3.5" />}
        label="Time"
        value={renderTime > 0 ? `${renderTime.toFixed(1)}s` : '—'}
      />
      <StatItem
        icon={<Hash className="h-3.5 w-3.5" />}
        label="Total Rays"
        value={totalRays > 0 ? formatNum(totalRays) : '—'}
      />
    </div>
  );
}

function StatItem({
  icon,
  label,
  value,
}: {
  icon: React.ReactNode;
  label: string;
  value: string;
}) {
  return (
    <div className="flex items-center gap-2 rounded-lg border border-border/30 bg-card/30 px-3 py-2">
      <span className="text-muted-foreground">{icon}</span>
      <div className="flex flex-col">
        <span className="text-[10px] uppercase tracking-wider text-muted-foreground/60">
          {label}
        </span>
        <span className="text-sm font-mono font-medium tabular-nums text-foreground">
          {value}
        </span>
      </div>
    </div>
  );
}
