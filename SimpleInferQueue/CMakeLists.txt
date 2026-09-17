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

        # ONNX Runtime（可选依赖）：只对真正用到它的示例生效——v1~v4 是纯 CUDA，
        # 绑上 ORT 会让它们在没装 ORT 的机器（如 CI）上构建失败。
        # 查找顺序：环境变量 ONNXRUNTIME_ROOT → third_party/onnxruntime → 都没找到
        # 就跳过依赖 ORT 的示例。默认值刻意不写死成某个绝对路径，否则这份文件
        # 换个机器就带着别人的 D 盘路径。
        # 本机路径写在 CMakeUserPresets.json（不入库），或用 -DONNXRUNTIME_ROOT= 覆盖。
        # 注意 ONNXRUNTIME_ROOT 是 CACHE 变量：一旦进了 CMakeCache.txt，改这里的默认值
        # 不会生效，必须显式 -D 覆盖或删掉缓存重配
        if(DEFINED ENV{ONNXRUNTIME_ROOT})
            set(_ort_default "$ENV{ONNXRUNTIME_ROOT}")
        else()
            set(_ort_default "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
        endif()
        set(ONNXRUNTIME_ROOT "${_ort_default}" CACHE PATH "ONNX Runtime 解压根目录")
        if(EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
            set(ONNXRUNTIME_AVAILABLE ON)
        else()
            set(ONNXRUNTIME_AVAILABLE OFF)
            message(STATUS "ONNX Runtime not found at ${ONNXRUNTIME_ROOT} - onnx examples will be skipped")
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

                # 按「源码内容」而非「文件名含 onnx」判断是否需要 ORT：
                # 文件改名/新增同类示例时不会漏配
                file(READ "${cuda_source}" cuda_example_content)
                string(FIND "${cuda_example_content}" "onnxruntime" cuda_ort_pos)

                if(cuda_ort_pos GREATER -1 AND NOT ONNXRUNTIME_AVAILABLE)
                    message(STATUS "CUDA example skipped (needs ONNX Runtime): ${cuda_example}")
                    continue()
                endif()

                add_executable(${cuda_example} ${cuda_source})
                target_link_libraries(${cuda_example} PRIVATE SimpleInferQueue::SimpleInferQueue)

                if(cuda_ort_pos GREATER -1)
                    target_include_directories(${cuda_example} PRIVATE "${ONNXRUNTIME_ROOT}/include")
                    target_link_libraries(${cuda_example} PRIVATE "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
                    # 模型路径以绝对路径注入：exe 在 build/Debug/ 下，源码里写
                    # "models/xxx.onnx" 这种相对路径会按「当前工作目录」解析，
                    # 从 build 目录运行就找不到模型，只有 cwd 恰好是仓库根才对。
                    # 内层引号必须转义：不转义的话 CMake 解析参数时会把这层引号
                    # 吃掉，宏展开后变成裸标记 D:/... 而不是字符串字面量，编译报错
                    #
                    # 注入 mobilenet_batch64.onnx（models/export_mobilenet_batch64.py 的产物），
                    # 不是 model.onnx / mobilenet.onnx——示例里 BATCH/IN_C/IN_H/IN_W/OUT_SIZE
                    # 是按 64x3x224x224 -> 64x1000 写死的，喂错模型会在 session.Run()
                    # 抛形状不匹配异常，而线程里没人接，直接 terminate 掉整个进程
                    # （且 stdout 是块缓冲，什么日志都看不到）
                    target_compile_definitions(${cuda_example} PRIVATE
                        "MODEL_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/models/mobilenet_batch64.onnx\"")
                    # ORT 的 DLL 必须与 exe 同目录：本机 System32 里残留着旧版
                    # onnxruntime.dll(1.17)，而 Windows 搜索顺序里 System32 优先于
                    # PATH，只配 PATH 会静默加载旧版并崩在 "requested API version
                    # ... is not available"。exe 自身目录排在 System32 之前，拷这里必胜。
                    # 用 GLOB 一次拷全：CPU 版只有 onnxruntime.dll，GPU 版还多出
                    # onnxruntime_providers_cuda.dll / _shared.dll，少拷一个就起不来
                    file(GLOB onnxruntime_dlls "${ONNXRUNTIME_ROOT}/lib/*.dll")
                    add_custom_command(TARGET ${cuda_example} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                ${onnxruntime_dlls}
                                "$<TARGET_FILE_DIR:${cuda_example}>"
                        COMMENT "Copying ONNX Runtime DLLs next to ${cuda_example}")
                    message(STATUS "CUDA example: ${cuda_example} (+ ONNX Runtime)")
                else()
                    message(STATUS "CUDA example: ${cuda_example}")
                endif()
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
