{
  description = "Snatcher (Sega CD) decomp / port build environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
        py = pkgs.python3.withPackages (ps: [ ps.capstone ]);
        # Windows cross toolchain (mingw-w64), used by tools/package/windows_cross.sh
        mingw = pkgs.pkgsCross.mingwW64.buildPackages.gcc;
      in {
        devShells.default = pkgs.mkShell {
          packages = [
            py pkgs.cmake pkgs.gnumake pkgs.ninja pkgs.pkg-config
            pkgs.sdl3          # native front-end (the Windows build fetches and links SDL3 statically)
            pkgs.ffmpeg        # PCM / CD-DA conversion for oracle work
            pkgs.ghidra        # Phase 0 disassembly project (tools/ghidra/setup_project.sh)
            pkgs.git
          ] ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ mingw ]
            ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ mingw pkgs.mame ];
          shellHook = ''
            # Use the stdenv compiler from this shell, never a stray ~/.nix-profile gcc (it cannot link against the macOS SDK).
            export CC=$(command -v ${if pkgs.stdenv.isDarwin then "clang" else "gcc"})
            export CXX=$(command -v ${if pkgs.stdenv.isDarwin then "clang++" else "g++"})
            echo "snatcher-port shell: cmake $(cmake --version | head -1 | cut -d' ' -f3), python $(python3 --version | cut -d' ' -f2)"
            echo "Disc images, BIOS dumps and extracted/ stay local (gitignored). See README.md."
          '';
        };
      });
}
