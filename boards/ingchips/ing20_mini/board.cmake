# SPDX-License-Identifier: Apache-2.0

board_runner_args(pyocd
  "--target=ing2000"
  "--dt-flash=n"
  "--erase"
  "--flash-opt=--no-reset"
  "--flash-opt=-O connect_mode=under-reset")
board_runner_args(jlink "--device=ING20xx" "--iface=swd" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
