{
  description = "Helios: a Sunshine fork for NixOS + KDE Plasma with per-session virtual displays, per-app audio, Steam library import and tuned defaults";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Upstream versions look like YYYY.MMDD.HHMMSS; derive ours from the commit time.
      date = self.lastModifiedDate or "19700101000000";
      version = "${builtins.substring 0 4 date}.${builtins.substring 4 4 date}.${builtins.substring 8 6 date}";
      rev = self.shortRev or self.dirtyShortRev or "unknown";

      mkPackage =
        pkgs:
        pkgs.callPackage ./nix/package.nix {
          src = self;
          inherit version rev;
        };
    in
    {
      packages = forAllSystems (pkgs: {
        default = mkPackage pkgs;
      });

      overlays.default = final: _prev: {
        helios = mkPackage final;
      };

      nixosModules.default = import ./nix/module.nix self;

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [ pkgs.ninja ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
