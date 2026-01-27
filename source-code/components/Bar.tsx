import React, { useState, useEffect } from 'react';
import { Wifi, Battery, Volume2, Cpu, MemoryStick as Memory, Clock } from 'lucide-react';
import { format } from 'date-fns';
import { HKConfig } from '../types';

interface BarProps {
  activeWorkspace: number;
  config: HKConfig;
}

const Bar: React.FC<BarProps> = ({ activeWorkspace, config }) => {
  const [time, setTime] = useState(new Date());

  useEffect(() => {
    const timer = setInterval(() => setTime(new Date()), 1000);
    return () => clearInterval(timer);
  }, []);

  return (
    <div 
        className="h-10 w-full backdrop-blur-md flex items-center justify-between px-6 text-xs select-none z-50 fixed top-0 left-0 right-0 transition-all duration-300"
        style={{
            backgroundColor: config.theme.bar_bg,
            borderBottom: `1px solid ${config.theme.border_inactive}`
        }}
    >
      
      {/* Workspaces */}
      <div className="flex items-center gap-4">
        <div 
            className="rounded px-3 py-1 font-bold tracking-widest uppercase text-[10px]"
            style={{ 
                backgroundColor: `${config.theme.accent_color}20`,
                color: config.theme.accent_color,
                border: `1px solid ${config.theme.accent_color}40`
            }}
        >
          HackerLand
        </div>
        <div className="flex gap-2">
          {[1, 2, 3, 4, 5].map((ws) => (
            <div
              key={ws}
              className={`
                w-2 h-2 rounded-full transition-all duration-300
                ${activeWorkspace === ws ? 'w-8' : 'hover:bg-slate-600'}
              `}
              style={{
                backgroundColor: activeWorkspace === ws ? config.theme.accent_color : '#334155'
              }}
            />
          ))}
        </div>
      </div>

      {/* Window Title */}
      <div className="text-slate-500 font-bold tracking-widest hidden md:block opacity-60">
        USER @ HACKEROS :: <span style={{ color: config.theme.accent_color }}>~/.config/HackerLand.hk</span>
      </div>

      {/* Status Modules */}
      <div className="flex items-center gap-6 font-medium">
        <div className="flex items-center gap-4 text-slate-400">
            <div className="flex items-center gap-2 hover:text-pink-400 transition-colors">
                <Cpu size={14} />
                <span>12%</span>
            </div>
            <div className="flex items-center gap-2 hover:text-purple-400 transition-colors">
                <Memory size={14} />
                <span>2.1G</span>
            </div>
        </div>
        
        <div className="h-4 w-px bg-white/10" />

        <div className="flex items-center gap-4 text-slate-400">
            <Wifi size={14} className="hover:text-green-400 transition-colors" />
            <Volume2 size={14} className="hover:text-blue-400 transition-colors" />
            <Battery size={14} className="hover:text-yellow-400 transition-colors" />
        </div>

        <div className="h-4 w-px bg-white/10" />

        <div 
            className="px-3 py-1 rounded flex items-center gap-2"
            style={{ color: config.theme.accent_color }}
        >
            <Clock size={14} />
            <span className="font-bold tracking-wider">{format(time, 'HH:mm')}</span>
        </div>
      </div>
    </div>
  );
};

export default Bar;
