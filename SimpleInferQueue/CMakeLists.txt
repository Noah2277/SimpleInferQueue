cmake_minimum_required(VERSION 3.16)
project(SimpleInferQueue VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /utf-8 是 MSVC 专属选项，GCC/Clang 不认识，必须按编译器区分；
# 并限定 C++ 语言——CUDA 目标会由 nvcc 收到不认识的 /utf-8（见下方 CUDA 示例块）
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
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

foreach(target demo_pc mock_inference)
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

# ============ 基准测试（Google Benchmark） ============
# 与测试相同的顶层检测逻辑：作为库被下游引入时不构建基准
option(SIMPLEINFERQUEUE_BUILD_BENCHMARKS "Build SimpleInferQueue benchmarks"
       ${SIMPLEINFERQUEUE_BUILD_TESTS_DEFAULT})

if(SIMPLEINFERQUEUE_BUILD_BENCHMARKS)
    include(FetchContent)
    FetchContent_Declare(
        googlebenchmark
        URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # 不编译 benchmark 自带的测试（省构建时间），也不随库安装到下游 prefix
    # （与 INSTALL_GTEST OFF 同理）
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(siq_benchmarks
        benchmarks/bench_block_queue.cpp
        benchmarks/bench_lockfree_queue.cpp
        benchmarks/bench_ms_queue.cpp)
    target_link_libraries(siq_benchmarks PRIVATE
        SimpleInferQueue::SimpleInferQueue
        benchmark::benchmark)
endif()

# ============ CUDA 示例（可选，需本机装有 CUDA Toolkit） ============
# CI 与无 GPU 机器上没有 CUDA，不能写进 project(LANGUAGES ...)，
# 用 check_language 探测：有则启用，没有则跳过
option(SIMPLEINFERQUEUE_BUILD_CUDA_EXAMPLES "Build CUDA pipeline examples"
       ${SIMPLEINFERQUEUE_BUILD_TESTS_DEFAULT})

if(SIMPLEINFERQUEUE_BUILD_CUDA_EXAMPLES)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)

        # 显式声明 CUDA 架构（CMake 4 起 CMP0104 默认 NEW，不允许留空）。
        # native = 为本机 GPU 直接编译 SASS（无 PTX JIT 开销，也消除 nvcc
        # 旧架构弃用警告）；无 GPU 的机器可用 -DCMAKE_CUDA_ARCHITECTURES=75 覆盖
        cmake_policy(SET CMP0104 NEW)
        if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES native)
        endif()

        # MSVC 宿主编译器：nvcc 默认按 GBK(936) 解读中文注释（C4819），
        # -Xcompiler=/utf-8 把该选项透传给 cl.exe（和手动 nvcc 命令一致）
        if(MSVC)
            set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=/utf-8")
        endif()

        # 自动发现 examples/cuda_pipeline/ 下的 .cu，每个文件一个可执行目标：
        # - file(GLOB CONFIGURE_DEPENDS) 监视「目录列表」——以后新增 .cu 文件时，
        #   cmake --build 会自动重新配置并加入新目标
        # - CMAKE_CONFIGURE_DEPENDS 监视「文件内容」——已存在的 .cu 被修改时
        #   同样触发自动重新配置（glob 只看文件名列表，不监视内容变化）
        file(GLOB cuda_pipeline_sources CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/examples/cuda_pipeline/*.cu")
        set_property(DIRECTORY APPEND PROPERTY
                     CMAKE_CONFIGURE_DEPENDS ${cuda_pipeline_sources})
        foreach(cuda_source ${cuda_pipeline_sources})
            # 空壳文件（还没有 main）跳过，写完内容后下一次构建自动加入
            # （file(SIZE) 必须用绝对路径——configure 阶段相对路径的解析基准不可靠）
            file(SIZE "${cuda_source}" cuda_example_size)
            if(cuda_example_size GREATER 0)
                get_filename_component(cuda_example "${cuda_source}" NAME_WE)
                add_executable(${cuda_example} ${cuda_source})
                target_link_libraries(${cuda_example} PRIVATE SimpleInferQueue::SimpleInferQueue)
                message(STATUS "CUDA example: ${cuda_example}")
            endif()
        endforeach()
    else()
        message(STATUS "CUDA compiler not found - skipping CUDA examples (CI runners have no GPU)")
    endif()
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
