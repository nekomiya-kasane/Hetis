cmake_minimum_required(VERSION 3.25)

set(Impeller_VERSION "1.4.0")
get_filename_component(Impeller_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(Impeller_FOUND FALSE)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
   OR CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|[Aa][Aa][Rr][Cc][Hh]")
    set(Impeller_NOT_FOUND_MESSAGE "The vendored Impeller SDK requires a Windows x64 target.")
    return()
endif()

if(NOT DEFINED Impeller_LINKAGE)
    set(Impeller_LINKAGE "shared")
endif()
if(NOT Impeller_LINKAGE STREQUAL "shared" AND NOT Impeller_LINKAGE STREQUAL "static")
    set(Impeller_NOT_FOUND_MESSAGE "Impeller_LINKAGE must be shared or static (got '${Impeller_LINKAGE}').")
    return()
endif()

foreach(_Impeller_file IN ITEMS include/impeller.h include/impeller.hpp
        lib/impeller.dll lib/impeller.dll.lib lib/impeller.lib bin/impellerc.exe)
    if(NOT EXISTS "${Impeller_ROOT_DIR}/${_Impeller_file}")
        set(Impeller_NOT_FOUND_MESSAGE "Incomplete Impeller SDK: missing ${Impeller_ROOT_DIR}/${_Impeller_file}")
        return()
    endif()
endforeach()
unset(_Impeller_file)

set(Impeller_IMPELLERC_EXECUTABLE "${Impeller_ROOT_DIR}/bin/impellerc.exe")

if(NOT TARGET Impeller::ImpellerShared)
    add_library(Impeller::ImpellerShared SHARED IMPORTED GLOBAL)
    set_target_properties(Impeller::ImpellerShared PROPERTIES
        IMPORTED_LOCATION "${Impeller_ROOT_DIR}/lib/impeller.dll"
        IMPORTED_IMPLIB "${Impeller_ROOT_DIR}/lib/impeller.dll.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${Impeller_ROOT_DIR}/include")
endif()

if(NOT TARGET Impeller::ImpellerStatic)
    add_library(Impeller::ImpellerStatic STATIC IMPORTED GLOBAL)
    set_target_properties(Impeller::ImpellerStatic PROPERTIES
        IMPORTED_LOCATION "${Impeller_ROOT_DIR}/lib/impeller.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${Impeller_ROOT_DIR}/include"
        INTERFACE_LINK_LIBRARIES "shlwapi;user32;advapi32;dbghelp;kernel32;uuid;rpcrt4;winmm")
endif()

if(NOT TARGET Impeller::impellerc)
    add_executable(Impeller::impellerc IMPORTED GLOBAL)
    set_target_properties(Impeller::impellerc PROPERTIES
        IMPORTED_LOCATION "${Impeller_IMPELLERC_EXECUTABLE}")
endif()

if(Impeller_LINKAGE STREQUAL "shared")
    set(_Impeller_target Impeller::ImpellerShared)
else()
    set(_Impeller_target Impeller::ImpellerStatic)
endif()
if(TARGET Impeller::Impeller)
    get_target_property(_Impeller_existing_target Impeller::Impeller ALIASED_TARGET)
    if(NOT _Impeller_existing_target STREQUAL _Impeller_target)
        set(Impeller_NOT_FOUND_MESSAGE "Impeller_LINKAGE cannot change after Impeller::Impeller has been created.")
        unset(_Impeller_existing_target)
        unset(_Impeller_target)
        return()
    endif()
    unset(_Impeller_existing_target)
else()
    add_library(Impeller::Impeller ALIAS ${_Impeller_target})
endif()
unset(_Impeller_target)

set(Impeller_FOUND TRUE)
