{
  description = "Hackerland Compositor and Bar";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
    rust-overlay.url = "github:oxalica/rust-overlay";
  };

  outputs = { self, nixpkgs, utils, rust-overlay }:
    utils.lib.eachDefaultSystem (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs { inherit system overlays; };

        # Zależności dla paska (Rust + GTK4 + Layer Shell)
        barDeps = with pkgs; [
          gtk4
          gtk4-layer-shell
          pkg-config
          glib
          cairo
          pango
          gdk-pixbuf
        ];

        # Zależności dla kompozytora (C++ + Mir/Miral)
        compositorDeps = with pkgs; [
          mir
          miral
          cmake
          pkg-config
          libxkbcommon
          wayland
          threads-access
        ];

      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "hackerland-full";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            rust-bin.stable.latest.default
            cargo
            wrapGAppsHook4 # Ważne dla GTK4: naprawia ikony i motywy
          ];

          buildInputs = barDeps ++ compositorDeps;

          # Logika budowania obu projektów
          buildPhase = ''
            # 1. Budujemy kompozytor (C++)
            mkdir -p build_cpp
            cd build_cpp
            cmake ..
            make -j$NIX_BUILD_CORES
            cd ..

            # 2. Budujemy pasek (Rust)
            # Uwaga: zakładamy, że main.rs jest w podkatalogu lub odpowiednio skonfigurowany
            cargo build --release
          '';

          installPhase = ''
            mkdir -p $out/bin
            
            # Instalacja binarek
            cp build_cpp/hackerland $out/bin/
            cp target/release/hackerland-bar $out/bin/
          '';
          
          # To zapewnia, że binarka znajdzie biblioteki Mir i GTK w runtime
          postFixup = ''
            wrapProgram $out/bin/hackerland --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.swaybg pkgs.kitty ]}
          '';
        };

        # Środowisko deweloperskie (Flox / nix develop)
        devShells.default = pkgs.mkShell {
          buildInputs = barDeps ++ compositorDeps ++ (with pkgs; [ rust-bin.stable.latest.default cargo ]);
          shellHook = ''
            export XDG_DATA_DIRS=${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:$XDG_DATA_DIRS
            echo "Hackerland Dev Environment Ready!"
          '';
        };
      });
}
