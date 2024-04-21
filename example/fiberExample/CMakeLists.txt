set(
    LIB_LIB
    fiber_lib
    pthread
    dl
)

add_executable(test_server server.cpp)
target_link_libraries(test_server ${LIB_LIB})

add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler ${LIB_LIB})

add_executable(test_iomanager test_iomanager.cpp)
target_link_libraries(test_iomanager ${LIB_LIB})

add_executable(test_hook test_hook.cpp)
target_link_libraries(test_hook ${LIB_LIB})