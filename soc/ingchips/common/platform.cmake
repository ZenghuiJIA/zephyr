# SPDX-License-Identifier: Apache-2.0

# A west workspace places the unmodified INGCHIPS SDK repository here. Keep
# ING_SDK_BASE as a cache variable so local SDK checkouts can still be used.
set(ING_SDK_BASE "${ZEPHYR_BASE}/../modules/hal/ingchips" CACHE PATH
    "Path to the INGCHIPS SDK repository")

if(CONFIG_SOC_ING9168)
  set(INGCHIPS_FAMILY ing916)
  set(INGCHIPS_BUNDLE ING9168xx)
elseif(CONFIG_SOC_ING20XX)
  set(INGCHIPS_FAMILY ing20)
  set(INGCHIPS_BUNDLE ING208xx)
elseif(CONFIG_SOC_ING9188)
  set(INGCHIPS_FAMILY ing918)
  set(INGCHIPS_BUNDLE ING9188xx)
else()
  message(FATAL_ERROR "Unsupported INGCHIPS SoC")
endif()

if(CONFIG_SOC_SERIES_ING20)
  zephyr_compile_definitions(INGCHIPS_ZEPHYR_SOC_ING20=1)
elseif(CONFIG_SOC_SERIES_ING916)
  zephyr_compile_definitions(INGCHIPS_ZEPHYR_SOC_ING916=1)
endif()

set(INGCHIPS_BUNDLE_DIR "${ING_SDK_BASE}/bundles/noos_mini/${INGCHIPS_BUNDLE}")
set(INGCHIPS_APIS_JSON "${INGCHIPS_BUNDLE_DIR}/apis.json")
set(INGCHIPS_META_JSON "${INGCHIPS_BUNDLE_DIR}/meta.json")
set(INGCHIPS_PLATFORM_BIN "${INGCHIPS_BUNDLE_DIR}/platform.bin")
set(INGCHIPS_STARTUP_DIR "${ING_SDK_BASE}/src/StartUP/${INGCHIPS_FAMILY}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${INGCHIPS_APIS_JSON}"
  "${INGCHIPS_META_JSON}")

zephyr_sources_ifdef(CONFIG_INGCHIPS_PLATFORM_OS
  "${CMAKE_CURRENT_LIST_DIR}/platform_os.c")

foreach(required_file IN ITEMS
    "${INGCHIPS_STARTUP_DIR}/ingsoc.h"
    "${INGCHIPS_APIS_JSON}"
    "${INGCHIPS_META_JSON}"
    "${INGCHIPS_PLATFORM_BIN}")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR
      "Missing INGCHIPS SDK file: ${required_file}. "
      "Run 'west update hal_ingchips' or set ING_SDK_BASE explicitly.")
  endif()
endforeach()

# Convert the SDK's exported platform ABI to GNU linker assignments accepted
# by --just-symbols.
file(READ "${INGCHIPS_APIS_JSON}" ingchips_apis_json)
string(JSON ingchips_api_count LENGTH "${ingchips_apis_json}")
set(INGCHIPS_SYMDEFS "${ZEPHYR_BINARY_DIR}/ingchips_symdefs.g")
file(WRITE "${INGCHIPS_SYMDEFS}"
  "/* Generated from ${INGCHIPS_APIS_JSON}; do not edit. */\n")
if(ingchips_api_count GREATER 0)
  math(EXPR ingchips_api_last "${ingchips_api_count} - 1")
  foreach(index RANGE ${ingchips_api_last})
    string(JSON symbol MEMBER "${ingchips_apis_json}" ${index})
    string(JSON address GET "${ingchips_apis_json}" "${symbol}")
    if(NOT address MATCHES "^0[xX][0-9a-fA-F]+$")
      message(FATAL_ERROR
        "Invalid address '${address}' for '${symbol}' in ${INGCHIPS_APIS_JSON}")
    endif()
    file(APPEND "${INGCHIPS_SYMDEFS}" "${symbol} = ${address};\n")
  endforeach()
endif()

# meta.json defines the boundary between the prebuilt platform and Zephyr.
file(READ "${INGCHIPS_META_JSON}" ingchips_meta_json)
string(JSON INGCHIPS_META_APP_BASE ERROR_VARIABLE meta_error
  GET "${ingchips_meta_json}" app base)
if(meta_error)
  message(FATAL_ERROR "Invalid ${INGCHIPS_META_JSON}: ${meta_error}")
endif()
string(JSON INGCHIPS_META_RAM_BASE GET "${ingchips_meta_json}" ram base)
string(JSON INGCHIPS_META_RAM_SIZE GET "${ingchips_meta_json}" ram size)
string(JSON INGCHIPS_META_ROM_BASE GET "${ingchips_meta_json}" rom base)
string(JSON INGCHIPS_META_ROM_SIZE GET "${ingchips_meta_json}" rom size)

math(EXPR ingchips_platform_rom_end
  "${INGCHIPS_META_ROM_BASE} + ${INGCHIPS_META_ROM_SIZE}")
math(EXPR ingchips_application_ram_base
  "(${INGCHIPS_META_RAM_BASE} + ${INGCHIPS_META_RAM_SIZE} + 7) & ~7")
file(SIZE "${INGCHIPS_PLATFORM_BIN}" ingchips_platform_bin_size)
if(ingchips_platform_bin_size GREATER INGCHIPS_META_ROM_SIZE)
  message(FATAL_ERROR
    "${INGCHIPS_PLATFORM_BIN} (${ingchips_platform_bin_size} bytes) exceeds "
    "the platform ROM region (${INGCHIPS_META_ROM_SIZE} bytes)")
endif()
if(ingchips_platform_rom_end GREATER INGCHIPS_META_APP_BASE)
  message(FATAL_ERROR
    "INGCHIPS platform ROM overlaps the application address in ${INGCHIPS_META_JSON}")
endif()

dt_chosen(ingchips_flash_path PROPERTY "zephyr,flash")
dt_chosen(ingchips_sram_path PROPERTY "zephyr,sram")
if(NOT DEFINED ingchips_flash_path OR NOT DEFINED ingchips_sram_path)
  message(FATAL_ERROR "INGCHIPS devicetree must select zephyr,flash and zephyr,sram")
endif()
dt_reg_addr(ingchips_dts_flash_base PATH "${ingchips_flash_path}")
dt_reg_addr(ingchips_dts_sram_base PATH "${ingchips_sram_path}")
math(EXPR ingchips_dts_flash_base_dec "${ingchips_dts_flash_base}"
  OUTPUT_FORMAT DECIMAL)
math(EXPR ingchips_meta_app_base_dec "${INGCHIPS_META_APP_BASE}"
  OUTPUT_FORMAT DECIMAL)
math(EXPR ingchips_dts_sram_base_dec "${ingchips_dts_sram_base}"
  OUTPUT_FORMAT DECIMAL)
math(EXPR ingchips_application_ram_base_dec "${ingchips_application_ram_base}"
  OUTPUT_FORMAT DECIMAL)
if(NOT "${ingchips_dts_flash_base_dec}" STREQUAL "${ingchips_meta_app_base_dec}")
  message(FATAL_ERROR
    "DTS flash base ${ingchips_dts_flash_base} does not match SDK application "
    "base ${INGCHIPS_META_APP_BASE}")
endif()
if(NOT "${ingchips_dts_sram_base_dec}" STREQUAL
    "${ingchips_application_ram_base_dec}")
  message(FATAL_ERROR
    "DTS SRAM base ${ingchips_dts_sram_base} does not match SDK application "
    "RAM base ${ingchips_application_ram_base}")
endif()

zephyr_include_directories(
  "${INGCHIPS_STARTUP_DIR}"
  "${ING_SDK_BASE}/src/BSP"
  "${ING_SDK_BASE}/src/FWlib"
  "${ING_SDK_BASE}/src/Tools"
  "${ING_SDK_BASE}/bundles/noos_mini/inc")
zephyr_sources_ifdef(CONFIG_SOC_ING20XX
  "${ING_SDK_BASE}/src/FWlib/peripheral_sysctrl.c")
target_link_options(app PUBLIC "-Wl,--just-symbols=${INGCHIPS_SYMDEFS}")

set_property(GLOBAL PROPERTY INGCHIPS_PLATFORM_BIN "${INGCHIPS_PLATFORM_BIN}")
set_property(GLOBAL PROPERTY INGCHIPS_PLATFORM_ROM_BASE "${INGCHIPS_META_ROM_BASE}")
set_property(GLOBAL PROPERTY INGCHIPS_APPLICATION_FLASH_BASE "${INGCHIPS_META_APP_BASE}")
configure_file("${INGCHIPS_PLATFORM_BIN}" "${ZEPHYR_BINARY_DIR}/platform.bin" COPYONLY)

set(INGCHIPS_PLATFORM_HEX "${ZEPHYR_BINARY_DIR}/platform.hex")
set(INGCHIPS_MERGED_HEX "${ZEPHYR_BINARY_DIR}/${KERNEL_NAME}_merged.hex")
set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
  COMMAND ${CMAKE_OBJCOPY}
    -I binary -O ihex
    --change-addresses=${INGCHIPS_META_ROM_BASE}
    "${ZEPHYR_BINARY_DIR}/platform.bin"
    "${INGCHIPS_PLATFORM_HEX}"
  COMMAND ${PYTHON_EXECUTABLE}
    "${ZEPHYR_BASE}/scripts/build/mergehex.py"
    -o "${INGCHIPS_MERGED_HEX}"
    "${INGCHIPS_PLATFORM_HEX}"
    "${ZEPHYR_BINARY_DIR}/${KERNEL_HEX_NAME}")
set_property(GLOBAL APPEND PROPERTY extra_post_build_byproducts
  "${INGCHIPS_PLATFORM_HEX}"
  "${INGCHIPS_MERGED_HEX}")

# Make west flash program the platform and Zephyr application in one operation.
set_target_properties(runners_yaml_props_target PROPERTIES
  hex_file "${KERNEL_NAME}_merged.hex")
