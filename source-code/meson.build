project('hackerland', 'c',
  version: '2.0.0',
  license: 'MIT',
  meson_version: '>=0.59.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false']
)

cc = meson.get_compiler('c')

# Enable unstable wlroots features
add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# Dependencies
wlroots = dependency('wlroots', version: '>=0.17.0')
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
math = cc.find_library('m')

# Optional XWayland support
if get_option('xwayland').enabled()
  wlr_xwayland = dependency('wlroots-xwayland', required: false)
  add_project_arguments('-DHAS_XWAYLAND=1', language: 'c')
else
  add_project_arguments('-DHAS_XWAYLAND=0', language: 'c')
endif

# --- Protocol Generation ---
wayland_scanner = find_program('wayland-scanner')
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

# Generator for header files (.h)
wayland_scanner_server = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

# Generator for code files (.c)
wayland_scanner_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

# Define protocols to generate
protocols = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
]

# Process protocols
generated_protocols = []
foreach p : protocols
  generated_protocols += wayland_scanner_server.process(p)
  generated_protocols += wayland_scanner_code.process(p)
endforeach

# --- Source files ---
src_files = files(
  'src/main.c',
  'src/server.c',
  'src/output.c',
  'src/input.c',
  'src/layout.c',
  'src/ui.c'
)

# Add generated protocols to source
src_files += generated_protocols

executable(
  'hackerland',
  src_files,
  dependencies: [
    wlroots,
    wayland_server,
    wayland_protos,
    xkbcommon,
    libinput,
    math
  ],
  install: true
)
