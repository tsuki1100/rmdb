set(SOURCES log_manager.cpp log_recovery.cpp)
add_library(recovery STATIC ${SOURCES})
target_link_libraries(recovery system pthread)
