// =============================================================================
//  netmech - 共享库导出宏
// =============================================================================
//  文件    : netmech/export.hpp
//  用途    : 让 netmech 既能以静态库、也能以**共享库/DLL**形式发布。
//  编码    : UTF-8
//
//  ★ 怎么用（构建方无需改代码）
//  -------------------------------------------------------------------------
//  构建时加 `-DNETMECH_LIBRARY_TYPE=SHARED` 即可；CMake 会自动：
//    · 给库本身定义 `NETMECH_BUILDING_LIBRARY`（导出符号）
//    · 给使用方定义 `NETMECH_SHARED`（导入符号）
//  头文件里只需在公共类/函数声明前写 `NETMECH_API`，其它情况展开为空。
//
//  ★ 为什么静态库时也要有这个宏
//  -------------------------------------------------------------------------
//  同一个头文件要同时服务三种形态：静态库、DLL 的导出侧、DLL 的导入侧。
//  宏在静态库下展开为空，不引入任何 ABI/性能负担。
// =============================================================================
#pragma once

// Windows：DLL 显式导出/导入；其它平台：默认即可（保持 ABI 面不变）。
//
// 非 Windows 上也可以配合 -fvisibility=hidden 用 NETMECH_API 收窄导出面，
// 但默认构建不做（Linux 两个共享库都走默认可见性，行为可预期）。
#if defined(_WIN32) && defined(NETMECH_SHARED)
#  if defined(NETMECH_BUILDING_LIBRARY)
#    define NETMECH_API __declspec(dllexport)
#  else
#    define NETMECH_API __declspec(dllimport)
#  endif
#else
#  define NETMECH_API
#endif
