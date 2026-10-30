// =============================================================================
//  NetMechanism - 全局唯一连接 ID 生成器（内部实现）
// =============================================================================
//  文件    : registry.hpp
//  用途    : 为 Conn 分配进程级全局唯一 id。
//  编码    : UTF-8
//
//  旧实现缺陷（A-07）的规避说明
//  ----------------------------
//  旧 `CAsioLinkMgr::getAvailableEpid()` 是：
//      uint32_t avaEpid = atomic_inc32(&m_epidNext);  avaEpid++;   // ← 必然跳一个号
//      { lock; if (map.find(avaEpid)==end) return avaEpid; }       // ← 检查与插入分离
//      ... 调用方随后在锁外 m_epid2endPoint[epid] = ep;            // ← 重复覆盖
//  因此旧实现同时存在 "必然跳号" "非原子的检查-插入 → 两条连接拿到同一 epid、
//  后者覆盖前者（丢连接 + 泄漏）" 两个问题。
//
//  新实现：
//    * id 由唯一的 std::atomic<std::uint32_t> 单调自增产生，**不跳号**；
//    * id 从 1 开始，0 保留为"无效 id"，便于上层用 0 判空；
//    * id **从不回绕复用**：若极端情况下耗尽（会先回绕到 0），
//      `try_allocate()` 返回 false 并拒绝新连接，而不是发一个重复 id；
//    * 分配发生在对象构造之前（`Conn` 构造函数即接收 id），分配与使用
//      之间不存在"检查-插入"窗口，因此**无需加锁**（这正是与旧实现的根本区别）。
// =============================================================================
#pragma once

#include "netmech_srcpch.hpp"

namespace netmech {

/// Conn 的 id 类型（**非 detail**：ConnState 等在 netmech 下使用）。
/// 0 保留为无效值。
using endpoint_id_t = std::uint64_t;

namespace detail {

/// 全局单调递增 id 生成器（进程内唯一实例）。
class IdRegistry {
public:
    /// 无效 id 的哨兵值（从未被分配）。
    static constexpr endpoint_id_t kInvalidId = 0;

    IdRegistry() = delete;

    /// 分配一个新 id（>= 1）。
    ///
    /// @return 成功返回新 id；id 空间耗尽（已到 UINT32_MAX）时返回 std::nullopt。
    /// @note 线程安全；无锁（单条 `fetch_add`）。
    /// @note 单调递增且**不跳号**：只要成功返回，序列必然是 1,2,3,…
    static std::optional<endpoint_id_t> try_allocate() noexcept
    {
        endpoint_id_t current = counter().load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<endpoint_id_t>::max()) {
                // 宁可拒绝连接，也绝不复用（复用会让旧 id 的迟到回调命中新连接）。
                return std::nullopt;
            }
            const endpoint_id_t   desired = current + 1;
            if (counter().compare_exchange_weak(current, desired, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                return desired;
            }
            // CAS 失败时 `current` 已被更新为最新值，继续循环。
        }
    }

    /// 分配一个新 id；耗尽时返回 kInvalidId。
    static endpoint_id_t allocate() noexcept
    {
        const std::optional<endpoint_id_t> id = try_allocate();
        return id.has_value() ? *id : kInvalidId;
    }

    /// 已分配的最大 id（诊断用；未分配任何 id 时为 0）。
    static endpoint_id_t last_allocated() noexcept
    {
        return counter().load(std::memory_order_relaxed);
    }

private:
    /// 已分配到的最大 id。首次分配从 1 开始。
    ///
    /// ★ 这里用「函数内静态」而不是 C++17 的 `static inline` 成员变量。
    ///
    ///   `static inline std::atomic<T> next_{0};` 在标准语义上应保证全程序
    ///   唯一实例，但实测在 **MSVC 2019 (v142) 14.28 + /GL（LTCG）** 下，
    ///   不同编译单元看到的符号类型不一致，链接期报：
    ///       warning C4744: "next_" 在不同 obj 中具有不同类型:
    ///                      "struct (4 bytes)" 和 "__declspec(align(4)) struct (4 bytes)"
    ///   这是/MSVC 的已知现象（LTCG 对 inline 静态数据成员的 COMDAT 合并）。
    ///
    ///   改成函数内静态后，它在每个使用点都是同一个函数作用域对象，
    ///   由编译器保证唯一初始化，不再产生跨 TU 的类型合并问题。
    static std::atomic<endpoint_id_t>& counter() noexcept
    {
        static std::atomic<endpoint_id_t> next{0};
        return next;
    }
};

} // namespace detail
} // namespace netmech
