# =============================================================================
#  aarch64 交叉编译工具链（GNU Toolchain for A-profile, 10.2-2020.11）
# =============================================================================
#  用法：
#      cmake -S . -B build-aarch64 \
#            -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
#            -DCMAKE_BUILD_TYPE=Release
#
#  可用 -DTOOLCHAIN_ROOT=<路径> 覆盖默认安装位置。
# =============================================================================
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_ROOT "/opt/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu"
    CACHE PATH "交叉编译工具链根目录")
set(TOOLCHAIN_PREFIX "aarch64-none-linux-gnu-"
    CACHE STRING "工具链前缀")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}g++")
set(CMAKE_AR           "${TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}ar")
set(CMAKE_RANLIB       "${TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}ranlib")
set(CMAKE_STRIP        "${TOOLCHAIN_ROOT}/bin/${TOOLCHAIN_PREFIX}strip")

# 不要设 CMAKE_SYSROOT：Arm 官方工具链自带 sysroot（-print-sysroot 会给出来），
# 再显式指定反而让 gcc 找不到 crt 文件（collect2: ld returned 1）。

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_ROOT}/${TOOLCHAIN_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # 工具链本身的程序在本机跑
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)    # 库只在 sysroot 里找
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
