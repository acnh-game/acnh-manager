cmake_minimum_required(VERSION 3.20)

project(hbl
    VERSION 3.0.0
    LANGUAGES ASM C
)

add_executable(hbl
    source/main.c
    source/trampoline.s
)

set_target_properties(hbl PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
)

target_link_options(hbl PRIVATE
    -Wl,-wrap,exit
)

target_compile_definitions(hbl PRIVATE
    VERSION="${CMAKE_PROJECT_VERSION}"
)

find_program(NX_ELF2NSO_EXE NAMES elf2nso HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
find_program(NX_NPDMTOOL_EXE NAMES npdmtool HINTS "${DEVKITPRO}/tools/bin" REQUIRED)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/exefs")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/exefs/main"
    COMMAND "${NX_ELF2NSO_EXE}" "$<TARGET_FILE:hbl>"
            "${CMAKE_CURRENT_BINARY_DIR}/exefs/main"
    DEPENDS hbl
    COMMENT "Converting hbl to NSO"
    VERBATIM
)

add_custom_target(hbl_nso ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/exefs/main"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/exefs/main.npdm"
    COMMAND "${NX_NPDMTOOL_EXE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/hbl.json"
            "${CMAKE_CURRENT_BINARY_DIR}/exefs/main.npdm"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hbl.json"
    COMMENT "Generating hbl NPDM"
    VERBATIM
)

add_custom_target(hbl_npdm ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/exefs/main.npdm"
)
