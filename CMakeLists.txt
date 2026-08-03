cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(nRF5340_Wearable)

target_sources(app PRIVATE
    src/main.c
    src/sen5x/sen5x_i2c.c
    src/sen5x/sensirion_i2c.c
    src/sen5x/sensirion_common.c
    src/sen5x/sensirion_i2c_hal.c
)
target_include_directories(app PRIVATE src/sen5x src/bmv080)

# BMV080: prebuilt Bosch libs (m33f hard-float). The two archives reference each
# other, so wrap them in a linker group so ld rescans until resolved.
target_link_libraries(app PRIVATE
    -Wl,--start-group
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bmv080/lib_bmv080.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bmv080/lib_postProcessor.a
    -Wl,--end-group
)
