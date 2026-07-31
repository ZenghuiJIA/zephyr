# SPDX-License-Identifier: Apache-2.0

function(ingchips_sdk_memory bundle flash_end sram_end)
  set(ING_SDK_BASE "${ZEPHYR_BASE}/../modules/hal/ingchips" CACHE PATH
      "Path to the INGCHIPS SDK repository")
  set(meta_json
      "${ING_SDK_BASE}/bundles/noos_mini/${bundle}/meta.json")

  if(NOT EXISTS "${meta_json}")
    message(FATAL_ERROR
      "Missing INGCHIPS SDK metadata: ${meta_json}. "
      "Run 'west update hal_ingchips' or set ING_SDK_BASE explicitly.")
  endif()

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${meta_json}")

  file(READ "${meta_json}" meta)
  string(JSON app_flash_base ERROR_VARIABLE meta_error GET "${meta}" app base)
  if(meta_error)
    message(FATAL_ERROR "Invalid ${meta_json}: ${meta_error}")
  endif()
  string(JSON platform_ram_base GET "${meta}" ram base)
  string(JSON platform_ram_size GET "${meta}" ram size)

  math(EXPR app_ram_base
    "(${platform_ram_base} + ${platform_ram_size} + 7) & ~7")
  math(EXPR app_flash_size "${flash_end} - ${app_flash_base}")
  math(EXPR app_ram_size "${sram_end} - ${app_ram_base}")

  if(app_flash_size LESS_EQUAL 0 OR app_ram_size LESS_EQUAL 0)
    message(FATAL_ERROR
      "Invalid application memory boundaries in ${meta_json}")
  endif()

  foreach(value IN ITEMS app_flash_base app_flash_size app_ram_base app_ram_size)
    math(EXPR ${value}_hex "${${value}}" OUTPUT_FORMAT HEXADECIMAL)
  endforeach()
  string(REGEX REPLACE "^0x" "" app_flash_base_unit "${app_flash_base_hex}")
  string(REGEX REPLACE "^0x" "" app_ram_base_unit "${app_ram_base_hex}")

  list(APPEND DTS_EXTRA_CPPFLAGS
    "-DINGCHIPS_APP_FLASH_BASE=${app_flash_base_hex}"
    "-DINGCHIPS_APP_FLASH_BASE_UNIT=${app_flash_base_unit}"
    "-DINGCHIPS_APP_FLASH_SIZE=${app_flash_size_hex}"
    "-DINGCHIPS_APP_RAM_BASE=${app_ram_base_hex}"
    "-DINGCHIPS_APP_RAM_BASE_UNIT=${app_ram_base_unit}"
    "-DINGCHIPS_APP_RAM_SIZE=${app_ram_size_hex}")
  set(DTS_EXTRA_CPPFLAGS "${DTS_EXTRA_CPPFLAGS}" PARENT_SCOPE)
endfunction()
