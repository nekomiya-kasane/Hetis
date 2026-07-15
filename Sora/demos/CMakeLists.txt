function(add_sora_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.Sora.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CATEGORY}/${DEMO_NAME}.Providers.cpp")
        target_sources(${DEMO_TARGET} PRIVATE "${CATEGORY}/${DEMO_NAME}.Providers.cpp")
    endif()
    target_include_directories(${DEMO_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${DEMO_TARGET} PRIVATE Sora Catch2::Catch2WithMain TBB::tbb)
    set_target_properties(${DEMO_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Demos/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${DEMO_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${DEMO_TARGET})
endfunction()

function(add_sora_demo_module CATEGORY MODULE_NAME)
    set(MODULE_TARGET Demo.Sora.${CATEGORY}.${MODULE_NAME})

    add_library(${MODULE_TARGET} SHARED ${CATEGORY}/${MODULE_NAME}.cpp)
    target_include_directories(${MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${MODULE_TARGET} PRIVATE Sora TBB::tbb)
    set_target_properties(${MODULE_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Demos/${CATEGORY}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
    vk_tp_deploy_runtime(${MODULE_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${MODULE_TARGET})
endfunction()

function(add_sora_asset_resource_library TARGET)
    add_library(${TARGET} SHARED ${ARGN})
    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    )
    target_compile_options(${TARGET} PRIVATE
        -fno-sanitize=address
        -fno-sanitize=undefined
        -Wno-c23-extensions
    )
    target_link_options(${TARGET} PRIVATE
        -fno-sanitize=address
        -fno-sanitize=undefined
    )
    if(WIN32)
        target_link_options(${TARGET} PRIVATE
            LINKER:/NOENTRY
            LINKER:/NODEFAULTLIB
        )
    endif()
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
        SUFFIX ".lpak"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )
endfunction()

function(add_sora_demo_asset_resource_library CATEGORY MODULE_NAME)
    set(MODULE_TARGET Demo.Sora.${CATEGORY}.${MODULE_NAME})

    add_sora_asset_resource_library(${MODULE_TARGET} ${CATEGORY}/${MODULE_NAME}.cpp)
    target_include_directories(${MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(${MODULE_TARGET} PROPERTIES FOLDER "Sora/Demos/${CATEGORY}")
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${MODULE_TARGET})
endfunction()

include(CTest)
include(Catch)

add_sora_demo(Core ABIDemo)
add_sora_demo(Core TraitsDemo)
add_sora_demo(Core PolymorphismDemo)
add_sora_demo(Core HashDemo)
add_sora_demo(Core ToStringDemo)
add_sora_demo(Core GetSetDemo)
add_sora_demo(Core CLIDemo)
add_sora_demo(Core StackTraceDemo)
add_sora_demo(Core AsyncEventCallbackDemo)

add_sora_demo_module(Core CLIModuleLinkedAlpha)
add_sora_demo_module(Core CLIModuleLinkedBeta)
add_sora_demo_module(Core CLIModulePluginGamma)
add_sora_demo_module(Core CLIModulePluginDelta)
add_sora_demo(Core CLIModuleDemo)
target_link_libraries(Demo.Sora.Core.CLIModuleDemo PRIVATE
    Demo.Sora.Core.CLIModuleLinkedAlpha
    Demo.Sora.Core.CLIModuleLinkedBeta
)
add_dependencies(Demo.Sora.Core.CLIModuleDemo
    Demo.Sora.Core.CLIModulePluginGamma
    Demo.Sora.Core.CLIModulePluginDelta
)
target_compile_definitions(Demo.Sora.Core.CLIModuleDemo PRIVATE
    SORA_CLI_PLUGIN_GAMMA="$<TARGET_FILE_NAME:Demo.Sora.Core.CLIModulePluginGamma>"
    SORA_CLI_PLUGIN_DELTA="$<TARGET_FILE_NAME:Demo.Sora.Core.CLIModulePluginDelta>"
)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.Help COMMAND Demo.Sora.Core.CLIModuleDemo --help)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.ShortHelp COMMAND Demo.Sora.Core.CLIModuleDemo -h)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.Host COMMAND Demo.Sora.Core.CLIModuleDemo host --details)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.LinkedAlpha
    COMMAND Demo.Sora.Core.CLIModuleDemo linked-alpha --value 17
)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.LinkedBeta COMMAND Demo.Sora.Core.CLIModuleDemo linked-beta --loud)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.PluginGamma COMMAND Demo.Sora.Core.CLIModuleDemo plugin-gamma Sora)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.PluginDelta
    COMMAND Demo.Sora.Core.CLIModuleDemo plugin-delta --repeat 4
)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.HostHelp COMMAND Demo.Sora.Core.CLIModuleDemo host --help)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.LinkedAlphaHelp COMMAND Demo.Sora.Core.CLIModuleDemo linked-alpha --help)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.LinkedBetaHelp COMMAND Demo.Sora.Core.CLIModuleDemo linked-beta --help)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.PluginGammaHelp COMMAND Demo.Sora.Core.CLIModuleDemo plugin-gamma --help)
add_test(NAME Demo.Sora.Core.CLIModuleDemo.PluginDeltaHelp COMMAND Demo.Sora.Core.CLIModuleDemo plugin-delta --help)
set_tests_properties(
    Demo.Sora.Core.CLIModuleDemo.Help
    Demo.Sora.Core.CLIModuleDemo.ShortHelp
    Demo.Sora.Core.CLIModuleDemo.Host
    Demo.Sora.Core.CLIModuleDemo.LinkedAlpha
    Demo.Sora.Core.CLIModuleDemo.LinkedBeta
    Demo.Sora.Core.CLIModuleDemo.PluginGamma
    Demo.Sora.Core.CLIModuleDemo.PluginDelta
    Demo.Sora.Core.CLIModuleDemo.HostHelp
    Demo.Sora.Core.CLIModuleDemo.LinkedAlphaHelp
    Demo.Sora.Core.CLIModuleDemo.LinkedBetaHelp
    Demo.Sora.Core.CLIModuleDemo.PluginGammaHelp
    Demo.Sora.Core.CLIModuleDemo.PluginDeltaHelp
    PROPERTIES LABELS "Sora;Demo;CLI"
)
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.Help PROPERTIES PASS_REGULAR_EXPRESSION "plugin-gamma")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.ShortHelp PROPERTIES PASS_REGULAR_EXPRESSION "linked-alpha")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.Host PROPERTIES PASS_REGULAR_EXPRESSION "executable: host")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.LinkedAlpha PROPERTIES PASS_REGULAR_EXPRESSION "linked-alpha DLL")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.LinkedBeta PROPERTIES PASS_REGULAR_EXPRESSION "linked-beta DLL")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.PluginGamma PROPERTIES PASS_REGULAR_EXPRESSION "plugin-gamma DLL")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.PluginDelta PROPERTIES PASS_REGULAR_EXPRESSION "plugin-delta DLL")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.HostHelp PROPERTIES PASS_REGULAR_EXPRESSION "--details")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.LinkedAlphaHelp
    PROPERTIES PASS_REGULAR_EXPRESSION "Usage: sora-cli-modules linked-alpha"
)
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.LinkedBetaHelp PROPERTIES PASS_REGULAR_EXPRESSION "--loud")
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.PluginGammaHelp
    PROPERTIES PASS_REGULAR_EXPRESSION "Usage: sora-cli-modules plugin-gamma"
)
set_tests_properties(Demo.Sora.Core.CLIModuleDemo.PluginDeltaHelp PROPERTIES PASS_REGULAR_EXPRESSION "--repeat")

add_sora_demo_module(Core CLIComplexLinkedA)
add_sora_demo_module(Core CLIComplexRuntimeB)
add_sora_demo_module(Core CLIComplexDiscoveredC)
add_sora_demo_module(Core CLIComplexNestedD)
add_sora_demo(Core CLIComplexModuleDemo)
target_link_libraries(Demo.Sora.Core.CLIComplexModuleDemo PRIVATE Demo.Sora.Core.CLIComplexLinkedA)
add_dependencies(Demo.Sora.Core.CLIComplexModuleDemo
    Demo.Sora.Core.CLIComplexRuntimeB
    Demo.Sora.Core.CLIComplexDiscoveredC
    Demo.Sora.Core.CLIComplexNestedD
)
target_compile_definitions(Demo.Sora.Core.CLIComplexModuleDemo PRIVATE
    SORA_CLI_COMPLEX_RUNTIME_B="$<TARGET_FILE_NAME:Demo.Sora.Core.CLIComplexRuntimeB>"
    SORA_CLI_COMPLEX_MANIFEST="sora-cli-complex.modules"
)
string(CONCAT SORA_CLI_COMPLEX_MANIFEST_CONTENT
    "$<TARGET_FILE_NAME:Demo.Sora.Core.CLIComplexDiscoveredC>|\n"
    "$<TARGET_FILE_NAME:Demo.Sora.Core.CLIComplexNestedD>|sub1\n"
)
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sora-cli-complex.modules"
    CONTENT "${SORA_CLI_COMPLEX_MANIFEST_CONTENT}"
)
unset(SORA_CLI_COMPLEX_MANIFEST_CONTENT)

add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.Help COMMAND Demo.Sora.Core.CLIComplexModuleDemo --help)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.Sub1Help
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub1 --help
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.NestedHelp
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub1 subsub1 --help
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.LinkedA
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub1 -vv --workspace . subsub2 --run input-a input-b
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.RuntimeB
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub2 --endpoint tcp.local submit --priority high a.job b.job
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.DiscoveredC
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub3 --namespace assets search --limit 5 triangle
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.NestedD
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo -vv sub1 -v --workspace . subsub1 -ss inspect scene.bin
            --format json --test
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.RequiredAncestor
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo sub1 subsub1 inspect scene.bin
)
add_test(NAME Demo.Sora.Core.CLIComplexModuleDemo.ForeignWorkingDirectory
    COMMAND Demo.Sora.Core.CLIComplexModuleDemo --help
)
set_tests_properties(
    Demo.Sora.Core.CLIComplexModuleDemo.Help
    Demo.Sora.Core.CLIComplexModuleDemo.Sub1Help
    Demo.Sora.Core.CLIComplexModuleDemo.NestedHelp
    Demo.Sora.Core.CLIComplexModuleDemo.LinkedA
    Demo.Sora.Core.CLIComplexModuleDemo.RuntimeB
    Demo.Sora.Core.CLIComplexModuleDemo.DiscoveredC
    Demo.Sora.Core.CLIComplexModuleDemo.NestedD
    Demo.Sora.Core.CLIComplexModuleDemo.RequiredAncestor
    PROPERTIES
        LABELS "Sora;Demo;CLI"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:Demo.Sora.Core.CLIComplexModuleDemo>"
)
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.Help PROPERTIES PASS_REGULAR_EXPRESSION "sub3")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.Sub1Help PROPERTIES PASS_REGULAR_EXPRESSION "subsub1")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.NestedHelp PROPERTIES PASS_REGULAR_EXPRESSION "inspect")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.LinkedA PROPERTIES PASS_REGULAR_EXPRESSION "linked A")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.RuntimeB PROPERTIES PASS_REGULAR_EXPRESSION "runtime B")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.DiscoveredC PROPERTIES PASS_REGULAR_EXPRESSION "discovered C")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.NestedD PROPERTIES PASS_REGULAR_EXPRESSION "nested D")
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.RequiredAncestor PROPERTIES
    WILL_FAIL TRUE
)
set_tests_properties(Demo.Sora.Core.CLIComplexModuleDemo.ForeignWorkingDirectory PROPERTIES
    LABELS "Sora;Demo;CLI"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "sub3"
)

add_sora_demo(PAL ModuleDemo)

add_sora_demo(Resources ResourcesDemo)
add_sora_demo_asset_resource_library(Resources EmbeddedPakModule)
add_sora_demo(Resources EmbeddedPakIndexDemo)
add_dependencies(Demo.Sora.Resources.EmbeddedPakIndexDemo Demo.Sora.Resources.EmbeddedPakModule)
target_compile_definitions(Demo.Sora.Resources.EmbeddedPakIndexDemo PRIVATE
    SORA_DEMO_EMBEDDED_PAK_MODULE_NAME="$<TARGET_FILE_NAME:Demo.Sora.Resources.EmbeddedPakModule>"
)

add_sora_demo(Kernel IIDDemo)
add_sora_demo(Kernel BaseObjectDemo)
add_sora_demo(Kernel CommandRuntimeDemo)

add_sora_demo(Core EventPortDemo)

add_sora_demo(Math PrimaryFunctionsDemo)
