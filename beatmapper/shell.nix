{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  name = "beatmapper-dev";

  nativeBuildInputs = with pkgs; [ gcc gnumake pkg-config ];

  buildInputs = with pkgs; [
    glfw
    # here lay all the inputs the AI thought u needed but u actually dont
    # libGL
    # libGLU
    # libX11
    # libXrandr
    # libXinerama
    # libXcursor
    # libXi
    # libXext

    gtk3
  ];

  shellHook = ''
    echo "beatmapper dev shell ready"
    echo "Run 'make' to build, 'make run' to build and run"

    # Ensure pkg-config can find glfw3
    export PKG_CONFIG_PATH="${pkgs.glfw}/lib/pkgconfig:$PKG_CONFIG_PATH"
  '';
}
