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

        # TensorRT / cuDNN（可选依赖）：只有源码里真的挂了 TensorRT EP 的示例才需要。
        # 编译期不需要 TRT 的 include 和导入库——OrtTensorRTProviderOptions 和
        # AppendExecutionProvider_TensorRT 都声明在 ORT 自己的头文件里
        # （onnxruntime_cxx_api.h），所以这里要解决的纯粹是「运行期 DLL 能被加载」。
        #
        # 为什么不配 PATH 而是拷到 exe 旁：机器上 PATH 里可能已经有别的软件自带的
        # nvinfer_10.dll（本机 HP OMEN Broadcast 就带了一个），谁先被搜到全看 PATH
        # 顺序；exe 同目录永远排在 System32 和 PATH 之前，拷这里必胜。
        # 本机路径同样写在 CMakeUserPresets.json（不入库），不写死在这份文件里。
        if(DEFINED ENV{TENSORRT_ROOT})
            set(_trt_default "$ENV{TENSORRT_ROOT}")
        else()
            set(_trt_default "")
        endif()
        set(TENSORRT_ROOT "${_trt_default}" CACHE PATH "TensorRT 解压根目录（DLL 在其 bin/ 下）")

        # builder resource 按 GPU 架构分文件（sm75/80/86/89/90/100/120/ptx），
        # 一个就有 260~450 MB，全拷要 1.9 GB，只能给本机架构那一份。
        # 本机 RTX 50 系是 Blackwell => 120；查自己的值：
        #   nvidia-smi --query-gpu=compute_cap --format=csv,noheader
        # 留空则不拷它：引擎缓存命中时不影响，首次现场构图会失败。
        set(TENSORRT_BUILDER_SM "" CACHE STRING "TensorRT builder resource 的 SM 版本号（如 120）")

        # cuDNN 也只要运行期可见。设了 CUDNN_ROOT 就拷到 exe 旁（565 MB），
        # 不设则靠 PATH。官方 zip 解出来的 DLL 在 bin/x64/，不是 bin/。
        set(CUDNN_ROOT "" CACHE PATH "cuDNN 解压根目录（DLL 在 bin/ 或 bin/x64/）")

        # TRT 的 DLL 几乎不静态依赖任何库（导入表里只有系统 DLL），
        # nvinfer_builder_resource_* / plugin 这些都是 nvinfer_10.dll 运行期 dlopen 的，
        # 所以必须和 nvinfer_10.dll 待在同一个目录，缺一个就在构图时炸
        # （实测：这 6 个 DLL 放进 exe 目录后，全新缓存现场构图能成功）
        set(TENSORRT_RUNTIME_DLLS "")
        if(EXISTS "${TENSORRT_ROOT}/bin/nvinfer_10.dll")
            foreach(_trt_dll nvinfer_10 nvonnxparser_10 nvinfer_plugin_10
                             nvinfer_lean_10 nvinfer_dispatch_10)
                if(EXISTS "${TENSORRT_ROOT}/bin/${_trt_dll}.dll")
                    list(APPEND TENSORRT_RUNTIME_DLLS "${TENSORRT_ROOT}/bin/${_trt_dll}.dll")
                endif()
            endforeach()
            if(TENSORRT_BUILDER_SM)
                set(_trt_builder "${TENSORRT_ROOT}/bin/nvinfer_builder_resource_sm${TENSORRT_BUILDER_SM}_10.dll")
                if(EXISTS "${_trt_builder}")
                    list(APPEND TENSORRT_RUNTIME_DLLS "${_trt_builder}")
                else()
                    message(WARNING "TENSORRT_BUILDER_SM=${TENSORRT_BUILDER_SM} 对应的 builder "
                                    "resource 不存在：首次现场构图会失败")
                endif()
            else()
                message(WARNING "TENSORRT_BUILDER_SM 未设置：首次现场构造 TRT 引擎会因缺 "
                                "nvinfer_builder_resource_sm<NN>_10.dll 失败")
            endif()
        endif()

        set(CUDNN_RUNTIME_DLLS "")
        if(CUDNN_ROOT)
            if(EXISTS "${CUDNN_ROOT}/bin/cudnn64_9.dll")
                set(_cudnn_dll_dir "${CUDNN_ROOT}/bin")
            elseif(EXISTS "${CUDNN_ROOT}/bin/x64/cudnn64_9.dll")
                set(_cudnn_dll_dir "${CUDNN_ROOT}/bin/x64")
            else()
                set(_cudnn_dll_dir "")
                message(WARNING "CUDNN_ROOT=${CUDNN_ROOT} 下没找到 cudnn64_9.dll（bin/ 或 bin/x64/）")
            endif()
            if(_cudnn_dll_dir)
                file(GLOB CUDNN_RUNTIME_DLLS "${_cudnn_dll_dir}/cudnn*.dll")
            endif()
        endif()

        # ============ TensorRT 原生封装库（可选） ============
        # src/tensorrt_engine.cpp 直接调 TensorRT 原生 API（#include <NvInfer.h>、
        # nvinfer1::），这和上面 v6 走 ORT 那条路有本质区别：v6 编译期完全不需要
        # TRT 的任何东西（OrtTensorRTProviderOptions 声明在 ORT 自己的头文件里），
        # 只要运行期 DLL 能被加载；这条要 include 目录 + 导入库，是真正的编译期依赖。
        #
        # 建 STATIC 库而不是把 .cpp 直接塞进每个示例，有两个原因：
        #   1. 它是独立组件，v7 和以后的版本共用
        #   2. 只有进了某个 target，VS Code 的 C/C++ 扩展（走 CMake File API）
        #      才能给它算出 include 路径。不在任何 target 里的文件，编辑器里连
        #      #include "tensorrt_engine.h" 都报「无法打开源文件」——这正是加这个
        #      目标之前的症状。
        set(SIMPLEINFERQUEUE_TRT_AVAILABLE OFF)
        if(EXISTS "${TENSORRT_ROOT}/include/NvInfer.h"
           AND EXISTS "${TENSORRT_ROOT}/lib/nvinfer_10.lib")
            # 跟随 FindCUDAToolkit 找到的那份 nvcc，要换用 -DCUDAToolkit_ROOT=
            find_package(CUDAToolkit REQUIRED)
            add_library(siq_trt STATIC src/tensorrt_engine.cpp)
            add_library(SimpleInferQueue::TRT ALIAS siq_trt)

            # tensorrt_engine.h 里 include 了 <NvInfer.h> 和 <cuda_runtime.h>，
            # 凡是 include 它的目标都得拿到这两条路径——所以是 PUBLIC 不是 PRIVATE
            target_include_directories(siq_trt PUBLIC "${TENSORRT_ROOT}/include")
            # SimpleInferQueue 是 INTERFACE 库，PUBLIC 链接把 include/ 一并传下去
            target_link_libraries(siq_trt PUBLIC
                SimpleInferQueue::SimpleInferQueue
                CUDA::cudart
                "${TENSORRT_ROOT}/lib/nvinfer_10.lib")
            set(SIMPLEINFERQUEUE_TRT_AVAILABLE ON)
            message(STATUS "TensorRT native wrapper: siq_trt (${TENSORRT_ROOT})")
        else()
            message(STATUS "TensorRT native wrapper skipped - "
                           "${TENSORRT_ROOT}/include/NvInfer.h 或 lib/nvinfer_10.lib 不存在")
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

                # 名字先打，后面的 "+ ..." 细节行才看得出属于谁。
                # 之前名字是最后打的，输出读起来像是挂在「前一个」示例下面
                if(cuda_ort_pos GREATER -1)
                    message(STATUS "CUDA example: ${cuda_example} (+ ONNX Runtime)")
                else()
                    message(STATUS "CUDA example: ${cuda_example}")
                endif()

                add_executable(${cuda_example} ${cuda_source})
                target_link_libraries(${cuda_example} PRIVATE SimpleInferQueue::SimpleInferQueue)

                # 按源码内容判断需要 TRT 的哪一层。两个标志独立判、不写成
                # if/else——两者并非互斥：_needs_trt_runtime 要拷运行期 DLL，
                # _needs_trt_native 要链原生封装库。
                # 这段必须在 ORT 分支**外面**：纯原生示例（直接用 TensorRTEngine
                # 读 .engine、源码不含 onnxruntime）走的是 else 分支，留在里面的话
                # 它既链不上 siq_trt、也拷不到 nvinfer_10.dll
                set(_trt_runtime_patterns
                    "OrtTensorRT"
                    "AppendExecutionProvider_TensorRT"
                    "tensorrt_engine.h")
                set(_trt_native_patterns
                    "tensorrt_engine.h"
                    "nvinfer1")

                set(_needs_trt_runtime OFF)
                foreach(_pat IN LISTS _trt_runtime_patterns)
                    string(FIND "${cuda_example_content}" "${_pat}" _trt_pos)
                    if(_trt_pos GREATER -1)
                        set(_needs_trt_runtime ON)
                    endif()
                endforeach()

                set(_needs_trt_native OFF)
                foreach(_pat IN LISTS _trt_native_patterns)
                    string(FIND "${cuda_example_content}" "${_pat}" _trt_pos)
                    if(_trt_pos GREATER -1)
                        set(_needs_trt_native ON)
                    endif()
                endforeach()

                # 原生路线只需链接：TRT 的 include 路径和导入库由 siq_trt
                # 以 PUBLIC 带出来，示例这侧不用重复写
                if(_needs_trt_native)
                    if(SIMPLEINFERQUEUE_TRT_AVAILABLE)
                        target_link_libraries(${cuda_example} PRIVATE SimpleInferQueue::TRT)
                    else()
                        message(WARNING "${cuda_example} 用了 TensorRT 原生 API，但 "
                                        "TENSORRT_ROOT 没配全（需要 include/NvInfer.h "
                                        "和 lib/nvinfer_10.lib）——这个示例会构建失败")
                    endif()
                endif()

                # 纯原生示例的模型路径。这里没有 ORT 分支那套「读源码里的 BATCH
                # 反推文件名」可用——原生示例源码里根本不写 BATCH，输入输出大小
                # 一律从 engine 自己查（input_size()/output_size()），所以无从推导，
                # 只能按 models/*.engine 取。
                # .engine 是 .gitignore 掉的派生产物（.gitignore:38），新克隆的仓库
                # 里一个都没有：必须 WARNING 说清楚，否则会安静地编出一个能跑、
                # 一跑才在消费者线程里 exit(1) 的 exe
                if(_needs_trt_native)
                    file(GLOB _engine_files "${CMAKE_CURRENT_SOURCE_DIR}/models/*.engine")
                    # GLOB 不保证顺序，排序后多引擎时选中的那个才可复现。
                    # 注意是字典序：batch128 会排在 batch32 前面
                    list(SORT _engine_files)
                    list(LENGTH _engine_files _engine_count)
                    if(_engine_count GREATER 0)
                        list(GET _engine_files 0 _engine_file)
                        target_compile_definitions(${cuda_example} PRIVATE
                            "ENGINE_PATH=\"${_engine_file}\"")
                        get_filename_component(_engine_name "${_engine_file}" NAME)
                        message(STATUS "  + engine: ${_engine_name}")
                        if(_engine_count GREATER 1)
                            message(STATUS "  + models/ 下有 ${_engine_count} 个 .engine，"
                                            "默认取字典序第一个；运行时可用 argv[1] 换")
                        endif()
                    else()
                        message(WARNING "${cuda_example} 要读 .engine，但 models/ 下没有——"
                                        "引擎由 .onnx 导出、不进版本库，先用 trtexec "
                                        "生成再跑这个示例")
                    endif()
                endif()

                # 运行期 DLL：原生示例要 nvinfer_10.dll，ORT 的 TRT EP 也要，同一份
                if(_needs_trt_runtime)
                    if(TENSORRT_RUNTIME_DLLS)
                        add_custom_command(TARGET ${cuda_example} POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                    ${TENSORRT_RUNTIME_DLLS}
                                    "$<TARGET_FILE_DIR:${cuda_example}>"
                            COMMENT "Copying TensorRT DLLs next to ${cuda_example}")
                        list(LENGTH TENSORRT_RUNTIME_DLLS _trt_dll_count)
                        message(STATUS "  + TensorRT runtime: ${_trt_dll_count} DLLs (sm${TENSORRT_BUILDER_SM})")
                    else()
                        # 不中断构建：ORT 路线下 TRT EP 的代码能编过（符号在 ORT 头里），
                        # 只是运行期挂 provider 会失败；纯原生路线则由上面的
                        # _needs_trt_native 分支报「构建失败」。两条都用 WARNING 说清楚
                        message(WARNING "${cuda_example} 需要 TensorRT 运行期，但 TENSORRT_ROOT "
                                        "没配（或 ${TENSORRT_ROOT}/bin 下没有 nvinfer_10.dll）——"
                                        "ORT 的 TRT EP 能编过，但运行时挂 provider 会失败")
                    endif()
                endif()

                if(cuda_ort_pos GREATER -1)
                    target_include_directories(${cuda_example} PRIVATE "${ONNXRUNTIME_ROOT}/include")
                    target_link_libraries(${cuda_example} PRIVATE "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
                    # 模型路径以绝对路径注入：exe 在 build/Debug/ 下，源码里写
                    # "models/xxx.onnx" 这种相对路径会按「当前工作目录」解析，
                    # 从 build 目录运行就找不到模型，只有 cwd 恰好是仓库根才对。
                    # 内层引号必须转义：不转义的话 CMake 解析参数时会把这层引号
                    # 吃掉，宏展开后变成裸标记 D:/... 而不是字符串字面量，编译报错
                    #
                    # 模型跟着源码里的 BATCH 常量走，而不是写死某个文件名：
                    # 示例里 BATCH/IN_C/IN_H/IN_W/OUT_SIZE 与模型规格本来就要求
                    # 一一对应，喂错会在 session.Run() 抛形状不匹配异常，而异常
                    # 发生在消费者线程里没人接，直接 terminate 掉整个进程，且
                    # stdout 是块缓冲，什么日志都看不到（这个坑踩过一次）。
                    # 自动配对消掉「改了 BATCH 忘了换模型」这一类崩溃，
                    # 也让 v5/v6 能各用各的 batch 而不用共用同一个宏
                    string(REGEX MATCH "BATCH[ \t]*=[ \t]*([0-9]+)" _batch_match "${cuda_example_content}")
                    if(_batch_match)
                        set(_model_file "mobilenet_batch${CMAKE_MATCH_1}.onnx")
                    else()
                        set(_model_file "mobilenet_batch64.onnx")
                    endif()
                    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/${_model_file}")
                        message(WARNING "${cuda_example} 的 BATCH 推出模型 ${_model_file}，"
                                        "但 models/ 下没有——先跑对应的导出脚本，"
                                        "否则运行时会直接 terminate 且无日志")
                    endif()
                    target_compile_definitions(${cuda_example} PRIVATE
                        "MODEL_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/models/${_model_file}\"")
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

                    # TRT 引擎缓存目录：ORT 的 TRT EP 专有，纯原生示例不用这个宏，
                    # 所以留在 ORT 分支里（siq_trt 链接和运行期 DLL 已移到分支外）。
                    # 必须从源码外注入绝对路径：源码里写 "./trt_cache" 会按「当前
                    # 工作目录」解析，从 build/ 跑就落到 build/ 下，和 MODEL_PATH
                    # 同一类坑。用 $<TARGET_FILE_DIR:> 而不是 ${CMAKE_CURRENT_BINARY_DIR}：
                    # 后者在多配置生成器（VS）下是 build 根，不是 build/Debug
                    if(_needs_trt_runtime)
                        target_compile_definitions(${cuda_example} PRIVATE
                            "TRT_CACHE_PATH=\"$<TARGET_FILE_DIR:${cuda_example}>/trt_cache\"")
                    endif()

                    # cuDNN 单独判、不放进 _needs_trt_runtime：那组关键词只命中 v6，
                    # 而走 CUDA EP 的 v5 也要 cuDNN。将来加了纯 CPU 的 ONNX 示例，
                    # 这里得补条件，否则白拷 565 MB
                    if(CUDNN_RUNTIME_DLLS)
                        add_custom_command(TARGET ${cuda_example} POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                    ${CUDNN_RUNTIME_DLLS}
                                    "$<TARGET_FILE_DIR:${cuda_example}>"
                            COMMENT "Copying cuDNN DLLs next to ${cuda_example}")
                        message(STATUS "  + cuDNN runtime DLLs")
                    endif()
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
