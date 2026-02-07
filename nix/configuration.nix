{ pkgs, ... }:
let
  hackerlandPkg = (builtins.getFlake "/sciezka/do/twojego/projektu").packages.${pkgs.system}.default;
in {
  environment.systemPackages = [ hackerlandPkg ];

  services.displayManager.sessionPackages = [
    (pkgs.writeTextDir "share/wayland-sessions/hackerland.desktop" ''
      [Desktop Entry]
      Name=Hackerland
      Comment=Mir-based Tiling Compositor
      Exec=${hackerlandPkg}/bin/hackerland
      Type=Application
    '')
  ];
}
