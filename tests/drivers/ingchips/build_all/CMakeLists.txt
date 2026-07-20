cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ingchips_driver_build_all)
target_sources(app PRIVATE src/main.c)
