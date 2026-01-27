import { FileSystem } from './types';

export const STARTUP_MESSAGE = [
  "HackerOS v1.0.0 (tty1)",
  " Kernel: 6.8.9-hacker-hardened",
  "",
  " Welcome to Hackerland.",
  " > Press Alt+Enter to open a Terminal",
  " > Press Alt+Q to close the active window",
  " > Press Alt+D to open Application Launcher",
  " > Press Alt+1...9 to switch workspaces",
  "",
].join("\n");

const DEFAULT_HK_FILE = `! HackerLand Configuration File
! Location: ~/.config/HackerLand.hk

[metadata]
-> name => HackerLand Defaults
-> version => 1.0

[theme]
-> border_active => #d946ef
-> border_inactive => #1e293b
-> blur_strength => 20px
-> gap_size => 16
-> outer_padding => 32
-> active_opacity => 1
-> inactive_opacity => 0.8
-> accent_color => #d946ef
-> bar_bg => #020617cc

[wallpaper]
-> url => https://images.unsplash.com/photo-1620641788421-7a1c342ea42e?q=80&w=1974&auto=format&fit=crop
-> overlay_opacity => 0.3

[animation]
-> duration => 0.4
-> stiffness => 120

[general]
-> font_family => JetBrains Mono
`;

export const MOCK_FILESYSTEM: FileSystem = {
  "home": {
    "user": {
      ".config": {
        "HackerLand.hk": DEFAULT_HK_FILE
      },
      "documents": {
        "manifesto.txt": "Information wants to be free.",
        "todo.txt": "- Build new kernel\n- Hack the planet"
      },
      "projects": {},
      "downloads": {}
    }
  },
  "etc": {
    "motd": "Have a lot of fun..."
  }
};
