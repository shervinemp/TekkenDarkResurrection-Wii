if(NOT NINTENDO_WII)
    return()
endif()

# Read-only NTFS-3G port for libogc devices (GPL-2+, vendored from
# rhyskoedijk/libntfs-wii).  Needed because libogc's FAT driver cannot read
# NTFS-formatted USB sticks, which is the only layout many players end up with.

file(GLOB REVC_NTFS_C_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c")

add_library(lwntfs STATIC)
target_sources(lwntfs PRIVATE ${REVC_NTFS_C_SOURCES})

target_include_directories(lwntfs
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source"
)

target_compile_definitions(lwntfs PRIVATE HAVE_CONFIG_H NTFS_ENABLE_LOG)

# The library predates devkitPPC GCC 15: gnu89 keeps the legacy C semantics it
# was written against (implicit int/declaration rules, rotated-with-long types)
# and -fcommon reconciles its pre-C11 tentative definitions.
set_target_properties(lwntfs PROPERTIES
    C_STANDARD 99 C_EXTENSIONS ON
)
target_compile_options(lwntfs PRIVATE -fcommon -fno-strict-aliasing)

target_link_libraries(lwntfs PUBLIC ogc)
