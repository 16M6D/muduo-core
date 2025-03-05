cmake_minimum_required(VERSION 2.5)
project(mymuduo)

# mymuduo 最终编译成.so动态库, 设置动态库的路径, 放在根目录的lib文件夹下
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

# 设置调试信息 ,用的语言标准C++11
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")

# 设置头文件路径, 让编译器能够找到头文件
include_directories(${PROJECT_SOURCE_DIR}/include)

# 设置源文件路径，编译源文件
aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_LIST)

# 编译生成动态库, STATIC默认静态
add_library(mymuduo SHARED ${SRC_LIST})
