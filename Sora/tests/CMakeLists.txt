add_library(Sora.TestMain OBJECT TestMain.cpp)
add_library(Sora.TestMain.NoCrashHandler OBJECT TestMain.cpp)
target_compile_definitions(Sora.TestMain.NoCrashHandler PRIVATE SORA_TEST_NO_CRASH_HANDLER=1)

function(add_sora_test TARGET_NAME)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "USE_MAIN;NO_CRASH_HANDLER" "CATAGORY" "SOURCES")

    if(ARG_CATAGORY)
        set(TARGET_NAME Sora.Test.${ARG_CATAGORY}.${TARGET_NAME})
        set(TARGET_FOLDER "Sora/Tests/${ARG_CATAGORY}")
    else()
        set(TARGET_NAME Sora.Test.${TARGET_NAME})
        set(TARGET_FOLDER "Sora/Tests")
    endif()

    add_executable(${TARGET_NAME} OBJECT ${ARG_SOURCES})
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TARGET_NAME} PRIVATE Sora Catch2::Catch2)
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER ${TARGET_FOLDER}
    )
    if(ARG_USE_MAIN)
        if(ARG_NO_CRASH_HANDLER)
            target_link_libraries(${TARGET_NAME} PRIVATE Sora.TestMain.NoCrashHandler)
        else()
            target_link_libraries(${TARGET_NAME} PRIVATE Sora.TestMain)
        endif()
    endif()

    message(STATUS "Added Sora test target: ${TARGET_NAME}")
endfunction()
