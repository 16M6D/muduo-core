#!/bin/bash

# 退出脚本时如果有错误发生
set -e

# 定义一些路径
SRC_DIR="src"
BUILD_DIR="build"
INCLUDE_DIR="include"

rm -rf build/
# 创建构建目录（如果不存在）
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR" && 
# 清理旧的
cmake ..

# 执行构建
echo "Building..."
make
# 输出构建结果
echo "Build completed."
#返回项目根目录
cd ..
# 将头文件拷贝到 /usr/include/mymuduo     so库拷贝到 /usr/lib
if [ ! -d /usr/include/mymuduo ]; then
    mkdir /usr/include/mymuduo
fi
cd ./include
for header in `ls *.h`
do 
    cp $header /usr/include/mymuduo
done

cd ..

cp `pwd`/lib/libmymuduo.so /usr/lib

ldconfig