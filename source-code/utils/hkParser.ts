import { HKConfig } from '../types';

export const DEFAULT_CONFIG: HKConfig = {
  theme: {
    border_active: '#06b6d4',
    border_inactive: '#334155',
    blur_strength: '16px',
    gap_size: 12,
    outer_padding: 24,
    active_opacity: 1,
    inactive_opacity: 0.85,
    accent_color: '#06b6d4',
    bar_bg: '#0f172acc',
  },
  wallpaper: {
    url: 'https://images.unsplash.com/photo-1550684848-fac1c5b4e853?q=80&w=2070&auto=format&fit=crop',
    overlay_opacity: 0.4,
  },
  animation: {
    duration: 0.3,
    stiffness: 100,
  },
  general: {
    font_family: 'JetBrains Mono',
  }
};

export const parseHKConfig = (fileContent: string): HKConfig => {
  const config = JSON.parse(JSON.stringify(DEFAULT_CONFIG)); // Deep copy default
  let currentSection = '';

  const lines = fileContent.split('\n');

  for (const line of lines) {
    const trimmed = line.trim();
    
    // Skip empty lines and comments
    if (!trimmed || trimmed.startsWith('!')) continue;

    // Section header [section]
    const sectionMatch = trimmed.match(/^\[(.*)\]$/);
    if (sectionMatch) {
      currentSection = sectionMatch[1].toLowerCase();
      continue;
    }

    // Key Value pair: -> key => value
    const kvMatch = trimmed.match(/^->\s+(.*?)\s+=>\s+(.*)$/);
    if (kvMatch && currentSection) {
      const key = kvMatch[1].trim();
      let value: string | number = kvMatch[2].trim();

      // Convert numbers
      if (!isNaN(Number(value)) && value !== '') {
        value = Number(value);
      }

      // Map to config object based on section
      if (config[currentSection]) {
        config[currentSection][key] = value;
      }
    }
  }

  return config;
};
