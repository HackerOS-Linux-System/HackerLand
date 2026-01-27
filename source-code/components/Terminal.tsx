import React, { useState, useRef, useEffect } from 'react';
import { STARTUP_MESSAGE } from '../constants';

interface TerminalProps {
  isActive: boolean;
}

const Terminal: React.FC<TerminalProps> = ({ isActive }) => {
  const [history, setHistory] = useState<string[]>(STARTUP_MESSAGE.split('\n'));
  const [input, setInput] = useState('');
  const bottomRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (isActive && inputRef.current) {
      inputRef.current.focus();
    }
  }, [isActive, history]);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [history]);

  const handleCommand = (cmd: string) => {
    const trimmed = cmd.trim();
    const args = trimmed.split(' ');
    const command = args[0].toLowerCase();

    let output = '';

    switch (command) {
      case 'help':
        output = "Available commands: help, clear, neofetch, whoami, ls, date, exit";
        break;
      case 'clear':
        setHistory([]);
        return;
      case 'whoami':
        output = "root@hackerland";
        break;
      case 'ls':
        output = "Desktop  Documents  Downloads  Music  Pictures  Videos";
        break;
      case 'date':
        output = new Date().toString();
        break;
      case 'neofetch':
        output = `
       /\\        OS: HackerOS x86_64
      /  \\       Host: Hackerland VM
     / /\\ \\      Kernel: 6.8.9-hardened
    / /  \\ \\     Uptime: ${Math.floor(performance.now() / 60000)} mins
   / /    \\ \\    Shell: zsh 5.9
  / /      \\ \\   DE: Hackerland (Web)
  \\/        \\/   Memory: 640KB / 64GB
`;
        break;
      case 'exit':
         output = "Cannot exit init process.";
         break;
      case '':
        output = "";
        break;
      default:
        output = `zsh: command not found: ${command}`;
    }

    setHistory(prev => [...prev, `➜  ~ ${cmd}`, output]);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      handleCommand(input);
      setInput('');
    }
  };

  return (
    <div 
      className="h-full w-full bg-[#0f0f15] text-green-400 p-4 font-mono text-sm overflow-hidden flex flex-col"
      onClick={() => inputRef.current?.focus()}
    >
      <div className="flex-1 overflow-y-auto whitespace-pre-wrap leading-tight">
        {history.map((line, i) => (
          <div key={i} className="min-h-[1.2em]">{line}</div>
        ))}
        <div ref={bottomRef} />
      </div>
      <div className="flex items-center mt-2 group">
        <span className="text-cyan-400 mr-2">➜</span>
        <span className="text-pink-400 mr-2">~</span>
        <input
          ref={inputRef}
          type="text"
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={handleKeyDown}
          className="bg-transparent border-none outline-none flex-1 text-gray-100 caret-white"
          autoFocus={isActive}
        />
      </div>
    </div>
  );
};

export default Terminal;
