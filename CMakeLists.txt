# SPDX-License-Identifier: MIT

if(CONFIG_KEYBALL_PEER_LAYER_SYNC)
  zephyr_library()
  # ZMK exposes its application headers privately to the app target. External
  # module libraries that subscribe to ZMK events must add the same path.
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_sources(src/peer_layer_sync.c)
endif()
