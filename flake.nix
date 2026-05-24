{
  description = "A flake for building and running PlotJuggler";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
          config.qt5.enable = true;
        };

        data-tamer-src = pkgs.fetchzip {
          url = "https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip";
          sha256 = "sha256-hGfoU6oK7vh39TRCBTYnlqEsvGLWCsLVRBXh3RDrmnY=";
        };

        plotjuggler-pkg = pkgs.qt5.mkDerivation {
          pname = "plotjuggler";
          version = "3.10.11";

          src = ./.;
          patches = [ ./nix/arrow.patch ];

          postPatch = ''
            substituteInPlace cmake/find_or_download_data_tamer.cmake \
              --replace-fail "URL" "SOURCE_DIR" \
              --replace-fail "https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip" "${data-tamer-src}"

            rm  cmake/find_or_download_fmt.cmake
            rm  cmake/find_or_download_zstd.cmake
            rm  cmake/find_or_download_lz4.cmake

            substituteInPlace CMakeLists.txt \
              --replace-fail "include(cmake/find_or_download_fmt.cmake)" "find_package(fmt REQUIRED)" \
              --replace-fail "find_or_download_fmt()" ""

            substituteInPlace CMakeLists.txt \
              --replace-fail 'include(''${PROJECT_SOURCE_DIR}/cmake/find_or_download_lz4.cmake)' "find_package(lz4 REQUIRED)" \
              --replace-fail "find_or_download_lz4()" ""

            substituteInPlace CMakeLists.txt \
              --replace-fail 'include(''${PROJECT_SOURCE_DIR}/cmake/find_or_download_zstd.cmake)' "find_package(zstd REQUIRED)" \
              --replace-fail "find_or_download_zstd()" ""

            # nixpkgs provides shared libs, not static
            find . -name "CMakeLists.txt" -exec sed -i 's/LZ4::lz4_static/LZ4::lz4_shared/g' {} +
            find . -name "CMakeLists.txt" -exec sed -i 's/zstd::libzstd_static/zstd::libzstd_shared/g' {} +

            # wasmer fails to build in nixpkgs; disable it (it's optional)
            substituteInPlace CMakeLists.txt \
              --replace-fail "include(cmake/download_wasmer.cmake)" "" \
              --replace-fail "download_wasmer()" ""
          '';

          cmakeFlags = [
            "-DPLJ_USE_SYSTEM_LUA=ON"
            "-DPLJ_USE_SYSTEM_NLOHMANN_JSON=ON"
          ];


          nativeBuildInputs = [ pkgs.cmake pkgs.qt5.wrapQtAppsHook ];

          buildInputs = [
            pkgs.qt5.full
            pkgs.qt5.qtsvg
            pkgs.qt5.qtimageformats
            pkgs.qt5.qtdeclarative
            pkgs.zeromq
            pkgs.sqlite
            pkgs.lua
            pkgs.nlohmann_json
            pkgs.fmt
            pkgs.lz4
            pkgs.zstd
            pkgs.mosquitto
            pkgs.protobuf
            pkgs.xorg.libX11
            pkgs.xorg.libxcb
            pkgs.xorg.xcbutil
            pkgs.xorg.xcbutilkeysyms
            pkgs.arrow-cpp
          ];

          meta = with pkgs.lib; {
            description = "A tool to plot streaming data, fast and easy";
            homepage = "https://github.com/facontidavide/PlotJuggler";
            license = licenses.mpl20;
            platforms = platforms.linux ++ platforms.darwin;
          };
        };

      in
      {
        packages.default = plotjuggler-pkg;
        packages.plotjuggler = plotjuggler-pkg;

        apps.default = {
          type = "app";
          program = "${plotjuggler-pkg}/bin/plotjuggler";
        };
        apps.plotjuggler = self.apps.${system}.default;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ plotjuggler-pkg ];
          packages = [
            pkgs.codespell
          ];
        };
      }
    );
}
