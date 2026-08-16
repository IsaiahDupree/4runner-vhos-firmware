get_filename_component(VHOS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
execute_process(
    COMMAND git describe --always --dirty --abbrev=12
    WORKING_DIRECTORY "${VHOS_REPO_ROOT}"
    OUTPUT_VARIABLE VHOS_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT VHOS_BUILD_ID)
    set(VHOS_BUILD_ID "source-tree")
endif()

idf_component_register(
    SRCS
        "main.c"
        "vhos_ble.c"
        "vhos_can.c"
        "vhos_transport.c"
    INCLUDE_DIRS "."
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE VHOS_BUILD_ID="${VHOS_BUILD_ID}")
