{
  lib,
  stdenv,
  runCommand,
  fetchurl,
  fetchNpmDeps,
  npmHooks,
  nodejs,
  cmake,
  pkg-config,
  python3,
  wayland-scanner,
  shaderc,
  autoPatchelfHook,
  qt6,
  boost,
  curl,
  miniupnpc,
  nlohmann_json,
  openssl,
  libopus,
  avahi,
  libevdev,
  libpulseaudio,
  libx11,
  libxcb,
  libxfixes,
  libxrandr,
  libxtst,
  libxi,
  libdrm,
  libgbm,
  libglvnd,
  libcap,
  libva,
  numactl,
  pipewire,
  glib,
  wayland,
  vulkan-headers,
  vulkan-loader,
  bash,
  coreutils,
  diffutils,

  # Provided by the flake.
  src,
  version,
  rev,
  desktop,
}:
let
  submodules = lib.importJSON ./submodules.json;

  # Upstream downloads these prebuilt, patched FFmpeg static libraries at configure time.
  # Pin the exact build-deps release the superproject references so the build stays offline.
  ffmpegBundle = fetchurl {
    url = "https://github.com/LizardByte/build-deps/releases/download/v2026.910.121303/Linux-x86_64-ffmpeg.tar.gz";
    hash = "sha256-SW0ru2dNAeYDPjG538FcvJ3BSU6IKkUF9qseA/dbOFw=";
  };
  ffmpeg = runCommand "sunshine-ffmpeg-prebuilt" { } ''
    mkdir -p $out
    tar -xzf ${ffmpegBundle} -C $out
  '';

  # The flake source carries no submodules; graft in only what a Linux build needs.
  fullSrc = runCommand "helios-src-${version}" { } (
    ''
      cp -r ${src} $out
      chmod -R u+w $out
    ''
    + lib.concatMapStrings (module: ''
      rm -rf $out/${module.path}
      cp -r ${
        builtins.fetchGit {
          inherit (module) url rev submodules;
          shallow = true;
        }
      } $out/${module.path}
      chmod -R u+w $out/${module.path}
    '') submodules
  );

  pythonWithJinja = python3.withPackages (ps: [ ps.jinja2 ]);
in
stdenv.mkDerivation (finalAttrs: {
  pname = "helios";
  inherit version;

  src = fullSrc;

  npmDeps = fetchNpmDeps {
    src = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../package.json
        ../package-lock.json
      ];
    };
    hash = "sha256-Jwbs9/hxz8n7WfbmobZLhHG2Rpjp1jFydyReF/zJBb4=";
  };

  nativeBuildInputs = [
    cmake
    pkg-config
    pythonWithJinja
    wayland-scanner
    shaderc
    nodejs
    npmHooks.npmConfigHook
    qt6.wrapQtAppsHook
    autoPatchelfHook
  ];

  buildInputs = [
    boost
    curl
    miniupnpc
    nlohmann_json
    openssl
    libopus
    avahi
    libevdev
    libpulseaudio
    libx11
    libxcb
    libxfixes
    libxrandr
    libxtst
    libxi
    libdrm
    libgbm
    libcap
    libva
    numactl
    pipewire
    glib
    wayland
    vulkan-headers
    vulkan-loader
    qt6.qtbase
    qt6.qtsvg
    qt6.qtwayland
  ];

  # Loaded with dlopen at runtime (EGL/GL through glad, avahi, gbm, X11 helpers).
  runtimeDependencies = [
    avahi
    libgbm
    libglvnd
    libxrandr
    libxcb
    vulkan-loader
  ];

  cmakeFlags = [
    "-Wno-dev"
    (lib.cmakeBool "BUILD_DOCS" false)
    (lib.cmakeBool "BUILD_TESTS" false)
    (lib.cmakeBool "BUILD_WERROR" false)
    (lib.cmakeBool "BOOST_USE_STATIC" false)
    (lib.cmakeBool "NPM_SKIP_INSTALL" true)
    (lib.cmakeBool "GLAD_SKIP_PIP_INSTALL" true)
    (lib.cmakeBool "SUNSHINE_SYSTEM_VULKAN_HEADERS" true)
    (lib.cmakeBool "SUNSHINE_ENABLE_CUDA" false)
    (lib.cmakeBool "CUDA_FAIL_ON_MISSING" false)
    (lib.cmakeFeature "FFMPEG_PREPARED_BINARIES" "${ffmpeg}/ffmpeg")
    (lib.cmakeFeature "SUNSHINE_ASSETS_DIR" "share/sunshine")
    (lib.cmakeFeature "SUNSHINE_EXECUTABLE_PATH" "${placeholder "out"}/bin/sunshine")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_NAME" "Helios")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_WEBSITE" "https://github.com/HalcyonOmega")
    (lib.cmakeFeature "SUNSHINE_PUBLISHER_ISSUE_URL" "https://github.com/HalcyonOmega")
    # Keep udev/systemd payloads inside $out instead of the systemd store path pkg-config reports.
    (lib.cmakeFeature "UDEV_RULES_INSTALL_DIR" "lib/udev/rules.d")
    (lib.cmakeFeature "SYSTEMD_USER_UNIT_INSTALL_DIR" "lib/systemd/user")
    (lib.cmakeFeature "SYSTEMD_SYSTEM_UNIT_INSTALL_DIR" "lib/systemd/system")
    (lib.cmakeFeature "SYSTEMD_MODULES_LOAD_DIR" "lib/modules-load.d")
  ];

  env = {
    # Version comes from the flake, not from git describe inside the sandbox.
    BUILD_VERSION = version;
    BRANCH = "main";
    COMMIT = rev;
  };

  # KWin grants its screencast protocol to an executable whose path appears in a desktop file
  # that KWin has indexed. Every update moves the binary to a new /nix/store path, and a running
  # Plasma session can keep refusing that path until the next login. `helios` therefore runs
  # the host from a fixed per-user copy, so its permission file stays valid across switches.
  postInstall = ''
    cat > $out/bin/helios <<EOF
    #!${bash}/bin/bash
    set -euo pipefail
    # Absolute tool paths: the host inherits PATH (apps such as \`steam\` are found through it).
    state="\''${XDG_STATE_HOME:-\$HOME/.local/state}/helios"
    target="\$state/sunshine"
    source="$out/bin/.sunshine-wrapped"
    ${coreutils}/bin/mkdir -p "\$state"
    if ! ${diffutils}/bin/cmp -s "\$source" "\$target"; then
      tmp=\$(${coreutils}/bin/mktemp "\$state/.sunshine.XXXXXX")
      ${coreutils}/bin/cp "\$source" "\$tmp"
      ${coreutils}/bin/chmod 0755 "\$tmp"
      ${coreutils}/bin/mv -f "\$tmp" "\$target"
    fi
    exec -a sunshine "\$target" "\$@"
    EOF
    chmod +x $out/bin/helios
    ln -s ${desktop}/bin/helios-desktop $out/bin/helios-desktop
    cp ${desktop}/share/applications/io.github.HalcyonOmega.Helios.desktop $out/share/applications/
    mkdir -p $out/share/icons/hicolor/scalable/apps
    cp ${desktop}/share/icons/hicolor/scalable/apps/helios.svg $out/share/icons/hicolor/scalable/apps/
  '';

  postFixup = ''
    # wrapQtAppsHook turns bin/sunshine into a launcher script. KWin grants the
    # screencast protocol by matching the client's real executable against this file.
    substituteInPlace $out/share/applications/dev.lizardbyte.app.Sunshine.kwin.desktop \
      --replace-fail "Exec=$out/bin/sunshine" "Exec=$out/bin/.sunshine-wrapped"

    # The NixOS module runs the user unit as `sunshine.service`.
    substituteInPlace $out/share/applications/dev.lizardbyte.app.Sunshine.desktop \
      --replace-fail "Exec=/usr/bin/env systemctl start --u app-dev.lizardbyte.app.Sunshine" "Exec=${desktop}/bin/helios-desktop"
    # Keep compatibility entries and KWin permissions but show only one branded app in menus.
    substituteInPlace $out/share/applications/dev.lizardbyte.app.Sunshine.desktop \
      --replace-fail "Type=Application" $'Type=Application\nNoDisplay=true'

    # wrapQtAppsHook only wraps ELF files; give the `helios` launcher the same environment so
    # the per-user copy it starts runs exactly like bin/sunshine.
    wrapQtApp $out/bin/helios
  '';

  passthru = {
    inherit ffmpeg;
    inherit desktop;
  };

  meta = {
    description = "Helios: Sunshine fork for NixOS and KDE Plasma with per-session virtual displays";
    homepage = "https://github.com/HalcyonOmega/helios";
    license = lib.licenses.gpl3Only;
    mainProgram = "helios";
    platforms = [ "x86_64-linux" ];
  };
})
