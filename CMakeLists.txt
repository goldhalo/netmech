# ============================================================================
#  NetMechanism —— netmech 网络通信库（C++17 / 独立 Asio / 无 C++20）
#
#  独立可构建工程：
#      cmake -S . -B build -G "Visual Studio 16 2019" -A x64
#      cmake --build build --config Release
#      ctest --test-dir build -C Release          （或直接跑 netmech_ut.exe）
#  也可被父工程 add_subdirectory() 引入。
#
#  依赖策略：
#      * 网络 IO 使用 **独立 Asio 1.30.2**（header-only），已随仓库放在
#        thirdparty/asio/include，**无需安装 Boost**。
#      * 仅依赖标准库 + Asio，无任何需要链接的第三方库。
#      * 使用 /MD 或 /MDd（动态 CRT）。
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(NetMechanism
        VERSION 0.9.0
        DESCRIPTION "netmech network library (standalone asio, C++17)"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 0. 全局编译标准
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # 关掉 gnu++17，保证严格 C++17

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 统一输出目录（便于脚本定位产物）
set(NETMECH_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         "${NETMECH_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${NETMECH_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${NETMECH_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/lib")

# 作为子工程被引入时默认不建测试/示例，由父工程决定
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NETMECH_TOP_LEVEL ON)
else()
    set(NETMECH_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# 1. 选项
# ---------------------------------------------------------------------------
option(NETMECH_BUILD_TESTS    "构建 NetMechanism 单元测试" ON)
option(NETMECH_BUILD_EXAMPLES "构建 NetMechanism 示例（echo server/client）" ON)
option(NETMECH_BUILD_BENCH    "构建 NetMechanism 性能基准（默认 OFF）" OFF)
option(NETMECH_WARNINGS_AS_ERRORS "把编译告警当错误（CI 用，默认 OFF）" OFF)

# ---------------------------------------------------------------------------
# 2. 第三方依赖：独立 Asio（header-only，随仓库提供）
# ---------------------------------------------------------------------------
#  为什么用独立 Asio 而非 Boost.Asio：
#    * 独立 Asio 直接使用 std::error_code，不依赖 Boost.System；
#    * header-only，**无需安装或链接任何第三方库**；
#    * 使用者无需安装 Boost，`git clone` 即可构建。
#
message(STATUS "   Asio 目录     : ${NETMECH_ASIO_INCLUDE_DIR}")
#  升级方式：替换 thirdparty/asio 目录即可（API 兼容 1.20+ 均可）。
set(NETMECH_ASIO_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/asio/include"
    CACHE PATH "独立 Asio 头文件目录（默认使用仓库内置版本）")

if(NOT EXISTS "${NETMECH_ASIO_INCLUDE_DIR}/asio.hpp")
    message(FATAL_ERROR
        "找不到独立 Asio：${NETMECH_ASIO_INCLUDE_DIR}/asio.hpp\n"
        "   仓库应自带 thirdparty/asio。若使用系统安装的 Asio，请设置：\n"
        "     cmake -DNETMECH_ASIO_INCLUDE_DIR=<asio 的 include 目录> ...")
endif()

# ---------------------------------------------------------------------------
# 3. 公共编译定义 / 选项（做成 interface 目标，库/测试/示例共享）
# ---------------------------------------------------------------------------
add_library(netmech_compile_options INTERFACE)

# Asio 是 header-only：只需把其 include 目录挂到 interface 目标上。


if(NETMECH_ASIO_INCLUDE_DIR)
    target_include_directories(netmech_compile_options SYSTEM INTERFACE ${NETMECH_ASIO_INCLUDE_DIR})
endif()

target_compile_definitions(netmech_compile_options INTERFACE
    _WIN32_WINNT=0x0601                         # Win7+ socket API
    WIN32_LEAN_AND_MEAN
    NOMINMAX


    _CRT_SECURE_NO_WARNINGS
)

# CRT：强制 /MD 或 /MDd，禁止 /MT
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    target_compile_options(netmech_compile_options INTERFACE
        /W4                     # 设计约定要求：四警告级
        /bigobj                 # 模板/asio 头文件实例化多
        /permissive-            # 严格标准一致性
        /Zc:__cplusplus         # 让 __cplusplus 正确反映 C++17
        /utf-8                  # 源码 UTF-8 + 中文注释（不依赖 BOM）
        /wd4819                 # C4819: "字符无法在当前代码页表示"
                                #   MSVC 2019 在已知源码为 UTF-8（/utf-8）时仍会对
                                #   部分字符串字面量报 C4819（报位还常有偏移），属工具
                                #   链噪音。源码本身校验为合法 UTF-8 + BOM，中文不会被
                                #   截断，故在此关闭该警告。保留 /utf-8 以确保真正按
                                #   UTF-8 解码（这才是防止 C4430 连锁的关键）。
        # C4459: 符号遮蔽。Asio 1.30 的 resolver_service.hpp 内部存在此类代码；
        #   属第三方头问题，且实例化由我们触发，故定向抑制（而非放宽 /W4）。
        /MP                     # 并行编译
        $<$<BOOL:${NETMECH_WARNINGS_AS_ERRORS}>:/WX>
    )
else()
    target_compile_options(netmech_compile_options INTERFACE
        -Wall -Wextra
        -finput-charset=UTF-8
        -fexec-charset=UTF-8
        $<$<BOOL:${NETMECH_WARNINGS_AS_ERRORS}>:-Werror>
    )
endif()

# 平台系统库
if(WIN32)
    # Windows socket API
    target_link_libraries(netmech_compile_options INTERFACE ws2_32 mswsock)
elseif(UNIX)
    # POSIX 线程：std::thread / std::mutex / std::condition_variable 都要 -pthread
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(netmech_compile_options INTERFACE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# 4. 源文件收集
# ---------------------------------------------------------------------------
set(NETMECH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

file(GLOB NETMECH_PUBLIC_HEADERS CONFIGURE_DEPENDS "${NETMECH_INCLUDE_DIR}/netmech/*.hpp")

set(NETMECH_SOURCES "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src")
    file(GLOB_RECURSE NETMECH_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")
endif()

if(NETMECH_SOURCES)
    add_library(NetMechanism STATIC ${NETMECH_SOURCES} ${NETMECH_PUBLIC_HEADERS})
    set(NETMECH_IS_INTERFACE OFF)
else()
    message(STATUS "NetMechanism: src/ 下暂无 .cpp，构建为 INTERFACE（header-only）库")
    add_library(NetMechanism INTERFACE)
    target_sources(NetMechanism INTERFACE ${NETMECH_PUBLIC_HEADERS})
    set(NETMECH_IS_INTERFACE ON)
endif()

set_target_properties(NetMechanism PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "NetMechanism"
    FOLDER "NetMechanism")

target_include_directories(NetMechanism PUBLIC
    $<BUILD_INTERFACE:${NETMECH_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>)

if(NOT NETMECH_IS_INTERFACE)
    target_include_directories(NetMechanism PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(NetMechanism PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PUBLIC_HEADER "${NETMECH_PUBLIC_HEADERS}")
endif()

# 独立 Asio 是 header-only：无需链接任何第三方库。
target_link_libraries(NetMechanism PUBLIC netmech_compile_options)

# ---------------------------------------------------------------------------
# 5. 头文件就绪性检查（并行开发期间给出清晰提示而不是一堆模板错误）
# ---------------------------------------------------------------------------
if(NOT EXISTS "${NETMECH_INCLUDE_DIR}/netmech")
    message(WARNING
        "NetMechanism: 公共头目录 ${NETMECH_INCLUDE_DIR}/netmech 尚不存在，"
        "测试/示例暂时无法编译。")
endif()

# ---------------------------------------------------------------------------
# 6. 单元测试
# ---------------------------------------------------------------------------
if(NETMECH_BUILD_TESTS)
    enable_testing()
    if(NOT EXISTS "${NETMECH_INCLUDE_DIR}/netmech/mechanism.hpp")
        message(WARNING "NetMechanism: 缺少 netmech/mechanism.hpp，跳过测试目标")
    else()
        file(GLOB NETMECH_TEST_SOURCES CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
        if(NETMECH_TEST_SOURCES)
            add_executable(netmech_ut ${NETMECH_TEST_SOURCES})
            set_target_properties(netmech_ut PROPERTIES FOLDER "NetMechanism/test")
            target_include_directories(netmech_ut PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test")
            target_link_libraries(netmech_ut PRIVATE NetMechanism)

            add_test(NAME netmech_ut COMMAND netmech_ut)
            set_tests_properties(netmech_ut PROPERTIES
                TIMEOUT 300
                WORKING_DIRECTORY "${NETMECH_OUTPUT_DIR}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# 6.5 性能基准（默认不构建）
# ---------------------------------------------------------------------------
#  用途：量化设计决策的性能影响（例如 strand 的有无）。
#  这些程序不是测试、不进 ctest；手动运行并对比输出。
#
#  用法：
#      bench_strand post plain
#      bench_strand post strand
#      bench_strand send plain
#      bench_strand send strand
# ---------------------------------------------------------------------------
if(NETMECH_BUILD_BENCH)
    file(GLOB NETMECH_BENCH_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")
    foreach(src ${NETMECH_BENCH_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        set_target_properties(${name} PROPERTIES FOLDER "NetMechanism/bench")
        target_link_libraries(${name} PRIVATE NetMechanism)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 7. 示例
#    * netmech_echo —— TCP echo server/client（演示 Mechanism/Endpoint）
#    * netmech_udp  —— UDP echo server/client（演示 Receiver/Session/Channel）
# ---------------------------------------------------------------------------
if(NETMECH_BUILD_EXAMPLES)
    if(EXISTS "${NETMECH_INCLUDE_DIR}/netmech/mechanism.hpp"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/example/echo_main.cpp")
        add_executable(netmech_echo example/echo_main.cpp)
        set_target_properties(netmech_echo PROPERTIES FOLDER "NetMechanism/example")
        target_link_libraries(netmech_echo PRIVATE NetMechanism)
    endif()

    if(EXISTS "${NETMECH_INCLUDE_DIR}/netmech/udp.hpp"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/example/udp_echo_main.cpp")
        add_executable(netmech_udp example/udp_echo_main.cpp)
        set_target_properties(netmech_udp PROPERTIES FOLDER "NetMechanism/example")
        target_link_libraries(netmech_udp PRIVATE NetMechanism)
    endif()
endif()

# ---------------------------------------------------------------------------
# 8. 安装
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
if(NOT NETMECH_IS_INTERFACE)
    install(TARGETS NetMechanism
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/netmech")
else()
    install(DIRECTORY "${NETMECH_INCLUDE_DIR}/netmech"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
            FILES_MATCHING PATTERN "*.hpp")
endif()

# ---------------------------------------------------------------------------
# 9. 配置摘要
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS " NetMechanism ${PROJECT_VERSION}")
message(STATUS "   C++ 标准      : ${CMAKE_CXX_STANDARD}")
message(STATUS "   构建类型      : ${CMAKE_BUILD_TYPE} ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "   NETMECH_BUILD_TESTS    : ${NETMECH_BUILD_TESTS}")
message(STATUS "   NETMECH_BUILD_EXAMPLES : ${NETMECH_BUILD_EXAMPLES}")
message(STATUS "   NETMECH_BUILD_BENCH    : ${NETMECH_BUILD_BENCH}")
message(STATUS "   Asio 目录     : ${NETMECH_ASIO_INCLUDE_DIR}")
message(STATUS "   输出目录      : ${NETMECH_OUTPUT_DIR}")
message(STATUS "----------------------------------------------------------")
