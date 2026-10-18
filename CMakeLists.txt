# SPDX-License-Identifier: MIT

if(CONFIG_KEYBALL_PEER_LAYER_SYNC)
  zephyr_library()
  zephyr_library_sources(src/peer_layer_sync.c)
endif()
