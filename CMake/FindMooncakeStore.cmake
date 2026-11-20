# 使用正式 C 头文件和共享库，不复制接口声明、不在运行时查找符号。
set(MOONCAKE_STORE_ROOT "" CACHE PATH "Mooncake Store SDK prefix")
find_path(MOONCAKE_STORE_INCLUDE_DIR NAMES store_c.h
    HINTS "${MOONCAKE_STORE_ROOT}"
    PATH_SUFFIXES include include/mooncake mooncake-store/include)

# 本接口明确使用动态库，避免误选同目录下的 libmooncake_store.a。
set(_mooncake_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
set(CMAKE_FIND_LIBRARY_SUFFIXES .so)
find_library(MOONCAKE_STORE_LIBRARY NAMES mooncake_store
    HINTS "${MOONCAKE_STORE_ROOT}"
    PATH_SUFFIXES lib lib64 mooncake-store/src/shared)
set(CMAKE_FIND_LIBRARY_SUFFIXES "${_mooncake_saved_suffixes}")
unset(_mooncake_saved_suffixes)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MooncakeStore REQUIRED_VARS
    MOONCAKE_STORE_INCLUDE_DIR MOONCAKE_STORE_LIBRARY)

if(MooncakeStore_FOUND AND NOT TARGET Mooncake::StoreC)
    if(NOT EXISTS "${MOONCAKE_STORE_INCLUDE_DIR}/store_c.h" OR
       NOT EXISTS "${MOONCAKE_STORE_LIBRARY}" OR
       NOT MOONCAKE_STORE_LIBRARY MATCHES "\\.so(\\.[0-9]+)*$")
        message(FATAL_ERROR "Mooncake requires store_c.h and a shared libmooncake_store.so")
    endif()
    add_library(Mooncake::StoreC SHARED IMPORTED)
    set_target_properties(Mooncake::StoreC PROPERTIES
        IMPORTED_LOCATION "${MOONCAKE_STORE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MOONCAKE_STORE_INCLUDE_DIR}")
endif()
mark_as_advanced(MOONCAKE_STORE_INCLUDE_DIR MOONCAKE_STORE_LIBRARY)
