cmake_minimum_required(VERSION 3.16)
project(SimpleInferQueue VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /utf-8 是 MSVC 专属选项，GCC/Clang 不认识，必须按编译器区分
if(MSVC)
    add_compile_options("/utf-8")
endif()

# 队列组件需要线程支持
find_package(Threads REQUIRED)

# ============ 库目标（header-only） ============
add_library(SimpleInferQueue INTERFACE)
add_library(SimpleInferQueue::SimpleInferQueue ALIAS SimpleInferQueue)
target_include_directories(SimpleInferQueue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(SimpleInferQueue INTERFACE Threads::Threads)

# ============ 示例程序 ============
add_executable(demo_pc examples/demo_producer_consumer.cpp)
add_executable(mock_inference examples/mock_inference.cpp)
add_executable(bench_blocking examples/bench_blocking.cpp)
add_executable(bench_lockfree examples/bench_lockfree.cpp)

foreach(target demo_pc mock_inference bench_blocking bench_lockfree)
    target_link_libraries(${target} PRIVATE SimpleInferQueue::SimpleInferQueue)
endforeach()

# ============ 测试（GoogleTest） ============
# 仅作为顶层项目构建时默认开启；被下游 FetchContent/add_subdirectory 引入时默认关闭
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SIMPLEINFERQUEUE_BUILD_TESTS_DEFAULT ON)
else()
    set(SIMPLEINFERQUEUE_BUILD_TESTS_DEFAULT OFF)
endif()
option(SIMPLEINFERQUEUE_BUILD_TESTS "Build SimpleInferQueue tests" ${SIMPLEINFERQUEUE_BUILD_TESTS_DEFAULT})

if(SIMPLEINFERQUEUE_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.16.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Windows 下避免 gtest 静态库与宿主程序 CRT 不一致的链接错误
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # gtest 是测试依赖，不应随库一起安装到下游 prefix
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)
    add_executable(siq_tests
        tests/test_ring_buffer.cpp
        tests/test_block_queue.cpp
        tests/test_lockfree_queue.cpp
        tests/test_ms_queue.cpp)
    target_link_libraries(siq_tests PRIVATE
        SimpleInferQueue::SimpleInferQueue
        GTest::gtest_main)
    gtest_discover_tests(siq_tests)
endif()

# ============ 安装与包导出 ============
include(GNUInstallDirs)

install(TARGETS SimpleInferQueue EXPORT SimpleInferQueueTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SimpleInferQueueTargets
    FILE SimpleInferQueueTargets.cmake
    NAMESPACE SimpleInferQueue::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimpleInferQueue)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/SimpleInferQueueConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SimpleInferQueueConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimpleInferQueue)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SimpleInferQueueConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/SimpleInferQueueConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SimpleInferQueueConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimpleInferQueue)
