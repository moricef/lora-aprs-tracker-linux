#!/bin/sh
# Build the headless EGL/GBM MapLibre snapshot tool on the Pi against the
# cross-compiled ARM64 archives. No LVGL, no GLFW.
set -e
ML=${ML:-/home/adrasec09/maplibre-native}
MLBUILD=${MLBUILD:-$ML/build-cross}

INC="-I$ML/include -I$ML/platform/default/include -I$ML/vendor/maplibre-native-base/include"
for d in "$ML"/vendor/maplibre-native-base/deps/*/include; do INC="$INC -I$d"; done
INC="$INC -I/usr/include/libdrm -I/usr/include/freetype2"

LIBS="$MLBUILD/libmbgl-core.a \
  $MLBUILD/libmbgl-vendor-parsedate.a \
  $MLBUILD/vendor/maplibre-tile-spec/cpp/libmlt-cpp.a \
  $MLBUILD/libmbgl-vendor-csscolorparser.a \
  $MLBUILD/libmbgl-harfbuzz.a \
  $MLBUILD/libmbgl-freetype.a \
  $MLBUILD/libmbgl-vendor-nunicode.a \
  $MLBUILD/libmbgl-vendor-sqlite.a"

g++ -std=gnu++20 -O2 -g -fno-rtti $INC proto_headless_egl.cpp $LIBS $LIBS \
  -o headless_egl \
  -lEGL -lGLESv2 -lgbm -lcurl -ljpeg -lpng -lz -lwebp -luv \
  -licuuc -licui18n -licudata -lsqlite3 -ldrm -lfreetype -lpthread -lrt -ldl
echo "built: headless_egl"
