//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.h
//
// Identification: src/include/buffer/lru_k_replacer.h
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <deque>
#include <limits>
#include <list>
#include <mutex>  // NOLINT
#include <unordered_map>
#include <vector>
#include "common/config.h"
#include "common/macros.h"

namespace bustub {

/**
 * LRUKReplacer 实现了 LRU-k 页面置换策略。
 *
 * LRU-k 算法会淘汰 backward k-distance 最大的 frame。
 * backward k-distance 定义为：当前时间戳与第 k 次最近访问时间戳之差。
 *
 * 访问次数少于 k 次的 frame，其 backward k-distance 视为 +∞。
 * 若多个 frame 的 backward k-distance 为 +∞，
 * 则退化为经典 LRU，选择最早访问的 frame。
 */
class LRUKReplacer {
 public:
  /**
   * @brief 构造一个新的 LRUKReplacer
   * @param num_frames replacer 可管理的最大 frame 数
   * @param k LRU-k 中的 k
   */
  explicit LRUKReplacer(size_t num_frames, size_t k);

  DISALLOW_COPY_AND_MOVE(LRUKReplacer);

  ~LRUKReplacer() = default;

  /**
   * @brief 找到 backward k-distance 最大的 frame 并将其淘汰
   * @param[out] frame_id 被淘汰的 frame id
   * @return 是否成功淘汰
   */
  auto Evict(frame_id_t *frame_id) -> bool;

  /**
   * @brief 记录 frame 的一次访问
   * @param frame_id 被访问的 frame id
   */
  void RecordAccess(frame_id_t frame_id);

  /**
   * @brief 设置 frame 是否可被淘汰
   * @param frame_id frame id
   * @param set_evictable 是否可淘汰
   */
  void SetEvictable(frame_id_t frame_id, bool set_evictable);

  /**
   * @brief 从 replacer 中移除指定 frame
   * @param frame_id frame id
   */
  void Remove(frame_id_t frame_id);

  /**
   * @brief 返回当前可淘汰 frame 的数量
   */
  auto Size() -> size_t;

 private:
  struct FrameInfo {
    std::deque<size_t> history_;  // 时间戳历史，最旧在前，最新在后，最多保留 k_ 个
    bool evictable_{false};
  };

  size_t current_timestamp_{0};
  size_t curr_size_{0};
  size_t replacer_size_;
  size_t k_;
  std::unordered_map<frame_id_t, FrameInfo> records_;
  std::mutex latch_;
};

}  // namespace bustub
