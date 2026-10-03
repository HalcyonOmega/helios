{
  lib,
  stdenvNoCC,
  python3,
  qt6,
  makeWrapper,
  systemd,
  src,
  version,
}:
let
  python = python3.withPackages (ps: [ ps.pyside6 ]);
in
stdenvNoCC.mkDerivation {
  pname = "helios-desktop";
  inherit version src;
  nativeBuildInputs = [ makeWrapper qt6.wrapQtAppsHook ];
  buildInputs = [ qt6.qtbase qt6.qtsvg qt6.qtwayland ];
  dontBuild = true;
  dontWrapQtApps = true;

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib/helios-desktop $out/bin $out/share/applications $out/share/icons/hicolor/scalable/apps
    cp tools/desktop/*.py $out/lib/helios-desktop/
    cp src_assets/common/helios.svg $out/share/icons/hicolor/scalable/apps/helios.svg
    cp src_assets/common/helios.svg $out/lib/helios-desktop/helios.svg
    makeWrapper ${python}/bin/python $out/bin/helios-desktop \
      --add-flags "$out/lib/helios-desktop/helios_desktop.py" \
      --add-flags "--systemctl ${systemd}/bin/systemctl --journalctl ${systemd}/bin/journalctl"
    cat > $out/share/applications/io.github.HalcyonOmega.Helios.desktop <<EOF
    [Desktop Entry]
    Type=Application
    Name=Helios
    GenericName=Moonlight Streaming Host
    Comment=Control your Moonlight streaming host
    Exec=$out/bin/helios-desktop
    TryExec=$out/bin/helios-desktop
    Icon=helios
    Categories=Network;RemoteAccess;
    Keywords=helios;sunshine;moonlight;streaming;remote play;
    StartupWMClass=helios-desktop
    StartupNotify=true
    Terminal=false
    EOF
    runHook postInstall
  '';

  postFixup = ''
    wrapProgram $out/bin/helios-desktop "''${qtWrapperArgs[@]}"
  '';

  passthru.python = python;
  meta = {
    description = "Helios native desktop controller for service, applications, devices, and streaming settings";
    license = lib.licenses.gpl3Only;
    platforms = [ "x86_64-linux" ];
    mainProgram = "helios-desktop";
  };
}
