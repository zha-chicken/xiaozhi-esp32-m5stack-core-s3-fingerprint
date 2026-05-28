# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.1.6")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

add_custom_target(patch_c6_ota_font_probe ALL
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/scripts/patch_c6_ota_font_probe.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Patch C6 OTA verifier font probe bytes"
    VERBATIM
)
add_dependencies(__idf_78__xiaozhi-fonts patch_c6_ota_font_probe)
