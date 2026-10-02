self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.stream-host;
in
{
  options.services.stream-host = {
    enable = lib.mkEnableOption "the stream-host Sunshine fork (wraps services.sunshine)";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "stream-host.packages.\${system}.default";
      description = "The stream-host package to run.";
    };
  };

  config = lib.mkIf cfg.enable {
    services.sunshine = {
      enable = true;
      package = cfg.package;
    };
  };
}
