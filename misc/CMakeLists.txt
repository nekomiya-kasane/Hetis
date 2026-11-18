set(DEBUGGER_VISUALIZERS
    "${CMAKE_CURRENT_SOURCE_DIR}/visualizers/libcxx.natvis"
    "${CMAKE_CURRENT_SOURCE_DIR}/lldb/init.lldb"
    "${CMAKE_CURRENT_SOURCE_DIR}/lldb/vulkan_lldb.py"
)
set_source_files_properties(${DEBUGGER_VISUALIZERS} PROPERTIES HEADER_FILE_ONLY TRUE)
add_custom_target(Hetis.DebuggerVisualizers SOURCES ${DEBUGGER_VISUALIZERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Misc" FILES ${DEBUGGER_VISUALIZERS})
