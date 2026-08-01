include_directories(/opt/homebrew/include/lua5.4 /opt/homebrew/include)
target_sources(dosboxcommon PRIVATE
  lua_api.cpp
  lua_bridge_commands.cpp
  lua_coroutine.cpp
  lua_debug_log.cpp
  lua_engine.cpp
  script_validator.cpp
  text_input.cpp
)

target_link_libraries(dosboxcommon PRIVATE
  ${LUA_LIBRARIES}
)
