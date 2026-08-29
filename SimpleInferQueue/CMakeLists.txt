cmake_minimum_required(VERSION 3.10)
project(SimpleInferQueue)
add_compile_options("/utf-8")

set(CMAKE_CXX_STANDARD 17)

#告诉编译器去哪里找你的头文件
include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(demo_pc examples/demo_producer_consumer.cpp)
add_executable(test_try examples/test_try.cpp)
add_executable(test_lockfree examples/test_lockfree.cpp)
add_executable(mock_inference examples/mock_inference.cpp)
add_executable(bench_blocking examples/bench_blocking.cpp)
add_executable(bench_lockfree examples/bench_lockfree.cpp)
add_executable(test_ms_queue examples/test_ms_queue.cpp)

#链接线程库
find_package(Threads REQUIRED)
target_link_libraries(demo_pc Threads::Threads)
target_link_libraries(test_try Threads::Threads)
target_link_libraries(test_lockfree Threads::Threads)
target_link_libraries(mock_inference Threads::Threads)
target_link_libraries(bench_blocking Threads::Threads)
target_link_libraries(bench_lockfree Threads::Threads)
target_link_libraries(test_ms_queue Threads::Threads)