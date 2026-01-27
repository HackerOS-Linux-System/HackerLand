import React from 'react';

export interface AppWindow {
  id: string;
  title: string;
  type: 'terminal' | 'browser' | 'settings' | 'welcome';
  content?: React.ReactNode;
}

export interface Workspace {
  id: number;
  windows: AppWindow[];
}

export type CommandHandler = (args: string[]) => string;

export interface FileSystem {
  [key: string]: string | FileSystem;
}

export interface HKConfig {
  theme: {
    border_active: string;
    border_inactive: string;
    blur_strength: string;
    gap_size: number;
    outer_padding: number;
    active_opacity: number;
    inactive_opacity: number;
    accent_color: string;
    bar_bg: string;
  };
  wallpaper: {
    url: string;
    overlay_opacity: number;
  };
  animation: {
    duration: number;
    stiffness: number;
  };
  general: {
    font_family: string;
  };
}
