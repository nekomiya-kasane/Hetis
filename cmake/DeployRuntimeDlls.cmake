if(NOT DEFINED VK_TP_RUNTIME_DESTINATION OR VK_TP_RUNTIME_DESTINATION STREQUAL "")
    message(FATAL_ERROR "VK_TP_RUNTIME_DESTINATION must name the executable directory")
endif()

if(CMAKE_ARGC LESS_EQUAL 4)
    return()
endif()

math(EXPR _vk_tp_last_argument "${CMAKE_ARGC} - 1")
foreach(_vk_tp_argument RANGE 4 ${_vk_tp_last_argument})
    set(_vk_tp_runtime_dll "${CMAKE_ARGV${_vk_tp_argument}}")
    if(_vk_tp_runtime_dll STREQUAL "")
        continue()
    endif()
    if(NOT EXISTS "${_vk_tp_runtime_dll}")
        message(FATAL_ERROR "Runtime DLL does not exist: ${_vk_tp_runtime_dll}")
    endif()

    cmake_path(GET _vk_tp_runtime_dll FILENAME _vk_tp_runtime_name)
    file(COPY_FILE "${_vk_tp_runtime_dll}" "${VK_TP_RUNTIME_DESTINATION}/${_vk_tp_runtime_name}"
         ONLY_IF_DIFFERENT)
endforeach()
