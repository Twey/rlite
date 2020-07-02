{ stdenv
, lib
, fetchFromGitHub
, runCommand
, which
, kmod
, protobuf
, cmake
, linux
, cacert
, python
, swig
, wpa_supplicant
, hostapd
, enable-python ? false
, enable-wifi ? false
}:
let
  deps = [ which cmake protobuf ];
in stdenv.mkDerivation rec {
  pname = "rlite";
  version = "2020-03-31";
  src = ./.;

  buildInputs = lib.concatLists [
    deps
    (lib.optionals enable-python [ python swig ])
    (lib.optionals enable-wifi [ wpa_supplicant hostapd ])
  ];

  postUnpack = "patchShebangs .";
  configurePhase = ''
    ./configure \
      --kernbuilddir "${linux.dev}/lib/modules/${linux.modDirVersion}/build" \
      --prefix $out \
  '';
  buildPhase = "make -j$NIX_BUILD_CORES usr";
  installPhase = "make usr_install";

  passthru.modules = stdenv.mkDerivation {
    pname = "${pname}-modules";
    inherit version src postUnpack configurePhase;
    buildInputs = deps ++ [ linux.dev kmod ];

    buildPhase = "make -j$NIX_BUILD_CORES ker";
    installPhase = "make ker_install";
  };
}
