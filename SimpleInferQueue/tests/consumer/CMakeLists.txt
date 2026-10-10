# 下游消费者的最小 CMake 工程。
#
# 它**不参与** SimpleInferQueue 本体的构建——本目录没有被父工程的 add_subdirectory 拉进去，
# 只能被单独 configure。CI 的 install-consumer job 就是这么用的：先 cmake --install 到一个
# 临时 prefix，再用这个工程把库 find_package 回来。
#
# 存在的理由：`INTERFACE_INCLUDE_DIRECTORIES` 被静默丢弃这类问题，**库自己构建得再好也照样
# 全绿**，只有真的「装一遍、再消费一遍」才暴露。下面这 4 行就是那种下游用户会写的全部内容，
# 多一行都不该有——一旦需要额外写 include_directories 才能编过，就说明包导出又坏了。
cmake_minimum_required(VERSION 3.16)
project(siq_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 用 -DCMAKE_PREFIX_PATH=<安装前缀> 指向 SimpleInferQueue 的安装位置
find_package(SimpleInferQueue CONFIG REQUIRED)

# 这里**故意不加任何编译选项**，尤其是父工程用的 MSVC /utf-8。
# main.cpp 因此写成纯 ASCII——它不是风格疏忽，而是这个测试的一部分：
# 下游工程必须能用**默认编译设置**消费本库。父工程加 /utf-8 是因为它的源码里有中文注释，
# 而 MSVC 默认按 936 代码页读源文件，某些汉字的 GBK 尾字节正好是 0x5C（反斜杠），
# 在 // 注释行末会被当成续行符、把下一行吞掉——实测就是「下一行的变量名未声明」。
# 那类文件确实需要 /utf-8；但**要求下游也加**是不合理的，所以这里反过来保证不需要。

add_executable(siq_consumer main.cpp)
target_link_libraries(siq_consumer PRIVATE SimpleInferQueue::SimpleInferQueue)
