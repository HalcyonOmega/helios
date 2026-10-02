self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.stream-host;

  # Opinionated defaults for a KDE Plasma desktop that streams to Moonlight on the LAN.
  # Every value can be overridden through `services.stream-host.settings`.
  defaultSettings = {
    # Give each Moonlight session its own KDE output at the client's resolution, and move
    # Steam game / Big Picture windows onto it, so the host's monitors stay usable.
    virtual_display = "enabled";
    virtual_display_move_windows = "enabled";

    # List every installed Steam game in Moonlight, with Steam's own cover art.
    steam_library = "enabled";

    # LAN streaming only: never punch holes in the router.
    upnp = "disabled";
    origin_web_ui_allowed = "lan";

    # Moonlight cannot grab the Super key on most clients; map Right Alt to it instead.
    key_rightalt_to_key_win = "enabled";

    notify_pre_releases = "disabled";
  };
in
{
  options.services.stream-host = {
    enable = lib.mkEnableOption "the stream-host Sunshine fork (configures services.sunshine)";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "stream-host.packages.\${system}.default";
      description = "The stream-host package to run.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Open the Moonlight streaming ports in the firewall.";
    };

    autoStart = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Start the host with the graphical session.";
    };

    settings = lib.mkOption {
      type = lib.types.attrsOf (
        lib.types.oneOf [
          lib.types.str
          lib.types.int
          lib.types.bool
        ]
      );
      default = { };
      example = {
        adapter_name = "/dev/dri/renderD128";
        virtual_display_scale = "2.0";
      };
      description = ''
        Settings merged over the tuned defaults and written to the Sunshine configuration
        file. Because the file is generated, the web UI shows these settings read-only.
        See <https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2configuration.html>.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    services.sunshine = {
      enable = true;
      inherit (cfg) package openFirewall autoStart;

      # KWin ScreenCast (used for virtual displays) needs no extra privileges; only KMS
      # capture does, and a capability wrapper would hide the real executable from KWin's
      # screencast permission check.
      capSysAdmin = false;

      settings = defaultSettings // cfg.settings;
    };
  };
}
