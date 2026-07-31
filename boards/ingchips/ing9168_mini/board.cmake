# SPDX-License-Identifier: Apache-2.0

board_runner_args(pyocd
  "--target=ing91600"
  "--dt-flash=n"
  "--erase"
  "--flash-opt=-O connect_mode=under-reset")
board_runner_args(jlink "--device=ING9168xx" "--iface=swd" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
