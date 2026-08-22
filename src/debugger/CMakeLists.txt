target_sources(dosboxcommon PRIVATE
  debugger_backtrace.cpp
  debugger_disasm.cpp)

if (OPT_DEBUGGER)
  target_sources(dosboxcommon PRIVATE
    debugger.cpp
    debugger_gui.cpp)

  target_link_libraries(dosboxcommon PRIVATE imgui)
endif()
