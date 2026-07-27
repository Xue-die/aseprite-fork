#!/bin/bash
mkdir -p build
cd build
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DENABLE_UI=ON \
  -DENABLE_DESKTOP_INTEGRATION=OFF \
  -DENABLE_SCRIPTING=ON \
  -DENABLE_SAVE=ON \
  -DENABLE_UPDATER=OFF \
  -DENABLE_TRIAL=OFF \
  -DENABLE_ALL_ALLEGRO=OFF \
  -DENABLE_ALLEGRO4=OFF \
  -DENABLE_DATA_RECOVERY=ON \
  -DENABLE_NEWS=ON \
  -DENABLE_WEBSOCKET=ON \
  -DLAF_BACKEND=skia \
  -DSKIA_DIR=$HOME/aseprite-fork/.deps/skia-m124 \
  -DSKIA_LIBRARY_DIR=$HOME/aseprite-fork/.deps/skia-m124/out/Release-arm64 \
  -DSKIA_LIBRARY=$HOME/aseprite-fork/.deps/skia-m124/out/Release-arm64/libskia.a \
  ..
ninja aseprite
