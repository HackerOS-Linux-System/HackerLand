import React, { useState, useEffect, useRef } from 'react';
import { Terminal, Globe, Settings, Folder } from 'lucide-react';
import { AppWindow } from '../types';
import { motion, AnimatePresence } from 'framer-motion';

interface LauncherProps {
  isOpen: boolean;
  onClose: () => void;
  onLaunch: (type: AppWindow['type']) => void;
  accentColor?: string;
}

const apps = [
  { id: 'term', name: 'Terminal', icon: Terminal, type: 'terminal' as const, desc: 'System Command Line' },
  { id: 'browser', name: 'Firefox', icon: Globe, type: 'browser' as const, desc: 'Web Browser' },
  { id: 'files', name: 'Thunar', icon: Folder, type: 'terminal' as const, desc: 'File Manager' },
  { id: 'settings', name: 'Settings', icon: Settings, type: 'settings' as const, desc: 'System Configuration' },
];

const Launcher: React.FC<LauncherProps> = ({ isOpen, onClose, onLaunch, accentColor = '#06b6d4' }) => {
  const [query, setQuery] = useState('');
  const [selectedIndex, setSelectedIndex] = useState(0);
  const inputRef = useRef<HTMLInputElement>(null);

  const filteredApps = apps.filter(app => 
    app.name.toLowerCase().includes(query.toLowerCase())
  );

  useEffect(() => {
    if (isOpen) {
      setTimeout(() => inputRef.current?.focus(), 50);
      setQuery('');
      setSelectedIndex(0);
    }
  }, [isOpen]);

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (!isOpen) return;

      if (e.key === 'ArrowDown') {
        e.preventDefault();
        setSelectedIndex(prev => (prev + 1) % filteredApps.length);
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        setSelectedIndex(prev => (prev - 1 + filteredApps.length) % filteredApps.length);
      } else if (e.key === 'Enter') {
        if (filteredApps[selectedIndex]) {
          onLaunch(filteredApps[selectedIndex].type);
          onClose();
        }
      } else if (e.key === 'Escape') {
        onClose();
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [isOpen, filteredApps, selectedIndex, onLaunch, onClose]);

  return (
    <AnimatePresence>
      {isOpen && (
        <motion.div 
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="fixed inset-0 z-[100] bg-black/60 backdrop-blur-sm flex items-start justify-center pt-[20vh]"
            onClick={onClose}
        >
          <motion.div 
            initial={{ y: -20, scale: 0.95 }}
            animate={{ y: 0, scale: 1 }}
            exit={{ y: -20, scale: 0.95 }}
            transition={{ type: "spring", stiffness: 300, damping: 30 }}
            className="w-[600px] bg-[#0f172a] border border-slate-700 rounded-xl shadow-2xl overflow-hidden flex flex-col"
            onClick={e => e.stopPropagation()}
            style={{ boxShadow: `0 0 50px -10px ${accentColor}20`, borderColor: `${accentColor}40` }}
          >
            <div className="p-4 border-b border-slate-800 flex items-center gap-4">
                <Terminal size={24} style={{ color: accentColor }} />
                <input 
                    ref={inputRef}
                    className="bg-transparent border-none outline-none text-white text-xl w-full placeholder-slate-600 font-light"
                    placeholder="Run command..."
                    value={query}
                    onChange={(e) => setQuery(e.target.value)}
                />
            </div>
            <div className="max-h-[400px] overflow-y-auto p-2 bg-[#020617]">
                {filteredApps.map((app, idx) => (
                    <div 
                        key={app.id}
                        className={`
                            flex items-center gap-4 p-3 rounded-lg cursor-pointer transition-all duration-200
                            ${idx === selectedIndex ? 'bg-slate-800' : 'hover:bg-slate-900'}
                        `}
                        style={{
                            borderLeft: idx === selectedIndex ? `3px solid ${accentColor}` : '3px solid transparent'
                        }}
                        onClick={() => {
                            onLaunch(app.type);
                            onClose();
                        }}
                    >
                        <div className={`p-2 rounded ${idx === selectedIndex ? 'bg-slate-700' : 'bg-slate-800'} transition-colors`}>
                            <app.icon size={20} className="text-slate-200" />
                        </div>
                        <div className="flex flex-col">
                            <span className={`font-medium ${idx === selectedIndex ? 'text-white' : 'text-slate-400'}`}>
                                {app.name}
                            </span>
                            <span className="text-xs text-slate-600">{app.desc}</span>
                        </div>
                        {idx === selectedIndex && (
                            <div className="ml-auto text-xs opacity-50 px-2 py-1 rounded bg-black/30">
                                ↵ Return
                            </div>
                        )}
                    </div>
                ))}
                {filteredApps.length === 0 && (
                    <div className="p-8 text-center text-slate-600 italic">
                        No matches found in $PATH
                    </div>
                )}
            </div>
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>
  );
};

export default Launcher;
