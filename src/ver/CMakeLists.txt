# Aseprite
# Copyright (C) 2021-2025  Igara Studio S.A.

set(VER_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/info.h
  )

target_sources(ver-lib PRIVATE ${VER_HEADERS})
target_compile_definitions(ver-lib PUBLIC
  -DASEPRITE_VERSION="${ASEPRITE_VERSION}"
  )
