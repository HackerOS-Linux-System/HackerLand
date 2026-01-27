import React, { useState, useEffect, useCallback, useMemo } from 'react';
import { AppWindow, HKConfig } from './types';
import Bar from './components/Bar';
import Terminal from './components/Terminal';
import Launcher from './components/Launcher';
import { MOCK_FILESYSTEM } from './constants';
import { DEFAULT_CONFIG, parseHKConfig } from './utils/hkParser';
import { X } from 'lucide-react';
import { AnimatePresence, motion } from 'framer-motion';

// Helper to safely access nested file system objects
const getFileContent = (path: string[], fs: any): string | null => {
  let current = fs;
  for (const part of path) {
    if (current[part] === undefined) return null;
    current = current[part];
  }
  return typeof current === 'string' ? current : null;
};

function App() {
  const [config, setConfig] = useState<HKConfig>(DEFAULT_CONFIG);
  const [activeWorkspace, setActiveWorkspace] = useState(1);
  const [windows, setWindows] = useState<AppWindow[]>([]);
  const [activeWindowId, setActiveWindowId] = useState<string | null>(null);
  const [isLauncherOpen, setIsLauncherOpen] = useState(false);

  // --- Load Config ---
  useEffect(() => {
    // Simulate reading from /home/user/.config/HackerLand.hk
    const configContent = getFileContent(['home', 'user', '.config', 'HackerLand.hk'], MOCK_FILESYSTEM);
    if (configContent) {
      try {
        const parsed = parseHKConfig(configContent);
        setConfig(parsed);
      } catch (e) {
        console.error("Failed to parse config", e);
      }
    }
  }, []);

  // --- Window Management ---

  const spawnWindow = useCallback((type: AppWindow['type']) => {
    const id = Math.random().toString(36).substr(2, 9);
    const title = type === 'terminal' ? '~/user/projects' : type;
    
    setWindows(prev => [...prev, { id, type, title }]);
    setActiveWindowId(id);
  }, []);

  const closeWindow = useCallback((id: string) => {
    setWindows(prev => {
      const remaining = prev.filter(w => w.id !== id);
      if (activeWindowId === id) {
         setActiveWindowId(remaining.length > 0 ? remaining[remaining.length - 1].id : null);
      }
      return remaining;
    });
  }, [activeWindowId]);

  const closeActiveWindow = useCallback(() => {
    if (activeWindowId) closeWindow(activeWindowId);
  }, [activeWindowId, closeWindow]);

  // --- Input Handling ---

  useEffect(() => {
    const handleGlobalKeys = (e: KeyboardEvent) => {
      if (!e.altKey) return;

      switch (e.key.toLowerCase()) {
        case 'enter': e.preventDefault(); spawnWindow('terminal'); break;
        case 'q': e.preventDefault(); closeActiveWindow(); break;
        case 'd': e.preventDefault(); setIsLauncherOpen(true); break;
        case '1': setActiveWorkspace(1); break;
        case '2': setActiveWorkspace(2); break;
        case '3': setActiveWorkspace(3); break;
        case '4': setActiveWorkspace(4); break;
        case '5': setActiveWorkspace(5); break;
      }
    };

    window.addEventListener('keydown', handleGlobalKeys);
    return () => window.removeEventListener('keydown', handleGlobalKeys);
  }, [spawnWindow, closeActiveWindow]);

  // --- Layout Calculations ---

  const renderWindows = () => {
    if (windows.length === 0) return null;

    const { gap_size, outer_padding } = config.theme;

    const containerStyle: React.CSSProperties = {
        display: 'flex',
        gap: `${gap_size}px`,
        padding: `${outer_padding}px`,
        paddingTop: `${outer_padding + 32}px`, // +Bar height
        height: '100vh',
        width: '100vw',
        boxSizing: 'border-box',
    };

    // Single Window
    if (windows.length === 1) {
        return (
            <div style={containerStyle}>
                <WindowFrame 
                    window={windows[0]} 
                    isActive={activeWindowId === windows[0].id} 
                    onClick={() => setActiveWindowId(windows[0].id)}
                    onClose={() => closeWindow(windows[0].id)}
                    config={config}
                />
            </div>
        );
    }

    // Master/Stack Layout
    const masterWindow = windows[0];
    const stackWindows = windows.slice(1);

    return (
        <div style={containerStyle}>
            {/* Master */}
            <motion.div layout className="flex-1 min-w-0 h-full">
                 <WindowFrame 
                    window={masterWindow} 
                    isActive={activeWindowId === masterWindow.id} 
                    onClick={() => setActiveWindowId(masterWindow.id)}
                    onClose={() => closeWindow(masterWindow.id)}
                    config={config}
                />
            </motion.div>

            {/* Stack */}
            <motion.div layout className="flex-1 min-w-0 h-full flex flex-col" style={{ gap: `${gap_size}px` }}>
                <AnimatePresence mode="popLayout">
                    {stackWindows.map((win) => (
                        <motion.div 
                            key={win.id} 
                            layout
                            initial={{ opacity: 0, x: 50 }}
                            animate={{ opacity: 1, x: 0 }}
                            exit={{ opacity: 0, scale: 0.9, transition: { duration: 0.2 } }}
                            className="flex-1 min-h-0"
                        >
                            <WindowFrame 
                                window={win} 
                                isActive={activeWindowId === win.id} 
                                onClick={() => setActiveWindowId(win.id)}
                                onClose={() => closeWindow(win.id)}
                                config={config}
                            />
                        </motion.div>
                    ))}
                </AnimatePresence>
            </motion.div>
        </div>
    );
  };

  return (
    <div 
        className="h-screen w-screen overflow-hidden bg-cover bg-center text-slate-200 transition-all duration-700 ease-in-out"
        style={{ 
            backgroundImage: `url(${config.wallpaper.url})`,
            fontFamily: config.general.font_family
        }}
    >
      <div 
        className="absolute inset-0 bg-black transition-opacity duration-700"
        style={{ opacity: config.wallpaper.overlay_opacity }} 
      />
      
      <Bar activeWorkspace={activeWorkspace} config={config} />
      
      <div className="relative z-0 h-full">
         <AnimatePresence mode="wait">
             {windows.length === 0 ? (
                 <motion.div 
                    key="empty"
                    initial={{ opacity: 0 }}
                    animate={{ opacity: 1 }}
                    exit={{ opacity: 0 }}
                    className="h-full w-full flex items-center justify-center text-slate-500/50 font-mono text-xl flex-col gap-4"
                 >
                     <div 
                        className="text-7xl font-bold tracking-tighter"
                        style={{ 
                            textShadow: `0 0 40px ${config.theme.accent_color}40`,
                            color: config.theme.accent_color,
                            opacity: 0.2
                        }}
                     >
                        HACKERLAND
                     </div>
                     <div className="flex flex-col items-center gap-2 text-sm uppercase tracking-widest opacity-60">
                        <span>Alt + Enter :: Terminal</span>
                        <span>Alt + D :: Launcher</span>
                        <span>Alt + Q :: Close</span>
                     </div>
                 </motion.div>
             ) : (
                 renderWindows()
             )}
         </AnimatePresence>
      </div>

      <Launcher 
        isOpen={isLauncherOpen} 
        onClose={() => setIsLauncherOpen(false)} 
        onLaunch={spawnWindow} 
        accentColor={config.theme.accent_color}
      />
    </div>
  );
}

const WindowFrame: React.FC<{
    window: AppWindow;
    isActive: boolean;
    onClick: () => void;
    onClose: () => void;
    config: HKConfig;
}> = ({ window, isActive, onClick, onClose, config }) => {
    
    return (
        <motion.div 
            onClick={onClick}
            initial={{ opacity: 0, scale: 0.95 }}
            animate={{ 
                opacity: isActive ? config.theme.active_opacity : config.theme.inactive_opacity,
                scale: 1,
                borderColor: isActive ? config.theme.border_active : config.theme.border_inactive,
                boxShadow: isActive ? `0 0 30px -5px ${config.theme.border_active}30` : 'none'
            }}
            transition={{ 
                duration: config.animation.duration,
                type: 'spring',
                stiffness: config.animation.stiffness
            }}
            style={{
                backdropFilter: `blur(${config.theme.blur_strength})`,
                WebkitBackdropFilter: `blur(${config.theme.blur_strength})`
            }}
            className={`
                h-full w-full rounded-lg overflow-hidden flex flex-col border
                bg-[#0f172a]/80 cursor-default
            `}
        >
            {/* Window Decoration */}
            <div 
                className="h-8 px-4 flex items-center justify-between text-xs select-none transition-colors duration-300"
                style={{
                    backgroundColor: isActive ? `${config.theme.accent_color}15` : 'rgba(0,0,0,0.2)',
                    color: isActive ? config.theme.accent_color : '#64748b'
                }}
            >
                <span className="font-bold tracking-wider uppercase flex items-center gap-2">
                    {isActive && <span className="w-1.5 h-1.5 rounded-full animate-pulse" style={{ backgroundColor: config.theme.accent_color }} />}
                    {window.title}
                </span>
                {isActive && (
                    <button onClick={(e) => { e.stopPropagation(); onClose(); }} className="hover:text-red-400 transition-colors p-1">
                        <X size={14} />
                    </button>
                )}
            </div>

            {/* Content */}
            <div className="flex-1 relative overflow-hidden bg-[#020617]/90">
                {window.type === 'terminal' ? (
                    <Terminal isActive={isActive} />
                ) : (
                    <div className="h-full w-full flex items-center justify-center text-slate-500 font-mono flex-col gap-4">
                        <div className="p-4 border border-dashed border-slate-700 rounded">
                             Application: <span style={{ color: config.theme.accent_color }}>{window.type}</span>
                        </div>
                        <span className="text-xs opacity-50">Rendering context not available</span>
                    </div>
                )}
            </div>
        </motion.div>
    );
}

export default App;
