{
  lib,
  stdenv,
  meson,
  ninja,
  cmake,
  hyprland,
  version ? "git",
}:
stdenv.mkDerivation rec {
  pname = "hyprscroller";
  inherit version;
  src = ./.;

  nativeBuildInputs = lib.filter (input: !lib.elem input [meson ninja]) hyprland.nativeBuildInputs ++ [cmake];

  buildInputs = [hyprland] ++ hyprland.buildInputs;

  buildPhase = ''
    make all
  '';

  installPhase = ''
    mkdir -p $out/lib/
    cp ${pname}.so $out/lib/lib${pname}.so
  '';

  meta = with lib; {
    homepage = "https://github.com/dawsers/hyprscroller";
    description = "Hyprland layout similar to PaperWM and niri";
    license = licenses.mit;
    platforms = platforms.linux;
  };
}
