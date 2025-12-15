### USAGE ###
# Run `nix-shell --run mk` to generate the compile_commands.json for the LSP
# Run `nix-shell --run bd` to build the whole engine for development

{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.ccache
    pkgs.pkg-config

    pkgs.clang
    pkgs.lldb

    pkgs.SDL2
    pkgs.openal
    pkgs.glew
    pkgs.glm
    pkgs.minizip
    pkgs.freetype
    pkgs.curl
    pkgs.p7zip
    pkgs.expat

    pkgs.fontconfig

    pkgs.libogg
    pkgs.libvorbis
    pkgs.libunwind
    pkgs.libdevil

    pkgs.xorg.libXi
    pkgs.xorg.libX11
    pkgs.xorg.libXrender
    pkgs.xorg.libXcursor
    pkgs.xorg.libXrandr
    pkgs.xorg.libXfixes
  ];

  CMAKE_PREFIX_PATH = pkgs.lib.makeSearchPath "lib/cmake" [
    pkgs.SDL2
    pkgs.libdevil
  ];

  cmakeFlags = [
    "-DBUILD_SHARED_LIBS=ON"
    "-DSDL2_USE_STATIC_LIBS=OFF"
  ];

# If getting a Thread error add the following lines to the cmake command
# -DCMAKE_USE_PTHREADS_INIT=ON \
# -DCMAKE_THREAD_LIBS_INIT="-lpthread" \

  shellHook = ''
    export CCACHE_DIR="$PWD/.cache/.ccache"
    mkdir -p "$CCACHE_DIR"

    DIR='build-linux'

    # Setup the build and obtain a fresh complie_commands.json for the lsp
    mk() {
      cmake -S . -B $DIR -GNinja \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_INSTALL_PREFIX="$PWD/$DIR/install" \
        -DCMAKE_C_COMPILER_LAUNCHER="ccache" \
        -DCMAKE_USE_PTHREADS_INIT=ON \
        -DCMAKE_THREAD_LIBS_INIT="-lpthread" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="ccache"

      cp -f "$DIR/compile_commands.json" compile_commands.json
    }

    # Actually build the /install contents for usage in the game
    bd() {
      mk
      ninja -C $DIR engine-legacy
      cmake --install $DIR
    }

    run() {
    }
  '';
}

