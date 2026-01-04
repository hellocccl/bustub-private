//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"

namespace bustub {

/**
 * 构造函数
 * @param num_frames Buffer Pool 中 frame 的最大数量
 * @param k LRU-K 中的 K，表示使用“倒数第 K 次访问”来计算淘汰优先级
 *
 * 注意：
 * - current_timestamp_ 和 curr_size_ 已在头文件中使用默认成员初始化
 * - 这里只初始化必须在构造阶段确定的成员
 */
LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {}

/**
 * Evict：选择backward k-distance最大的可以被淘汰的 frame 删除 并返回其 frame_id
 *
 * 淘汰规则（严格符合 LRU-K 定义）：
 * 1. 只考虑 evictable_ == true 的 frame
 * 2. 若某 frame 的访问次数 < k，则其 backward k-distance = +∞
 *    - 多个 +∞ 时，选择“最近一次访问时间最早”的（退化为 LRU）
 * 3. 若访问次数 ≥ k：
 *    - backward k-distance = current_timestamp - 第 k 次最近访问时间
 *    - 选择 backward k-distance 最大的
 *    - 若相同，仍以“最近一次访问时间最早”为准
 */
auto LRUKReplacer::Evict(frame_id_t *frame_id) -> bool {
  // 加锁，保证线程安全
  std::scoped_lock<std::mutex> lock(latch_);

  // 当前没有任何可淘汰的 frame
  if (curr_size_ == 0) {
    return false;
  }

  bool found = false;             // 是否已经找到候选 frame
  frame_id_t best_fid = -1;       // 当前最优候选 frame_id
  bool best_is_infinite = false;  // 当前最优候选是否为 +∞ backward distance
  size_t best_distance = 0;       // 当前最优候选的 backward k-distance
  size_t best_last_access = 0;    // 当前最优候选最近一次访问时间（用于 tie-break）

  // 遍历所有记录的 frame
  for (auto &entry : records_) {
    frame_id_t fid = entry.first;
    FrameInfo &info = entry.second;

    // 不可淘汰的 frame 直接跳过
    if (!info.evictable_) {
      continue;
    }

    // 若历史访问次数 < k，则 backward k-distance 视为 +∞
    bool is_infinite = info.history_.size() < k_;

    // 最近一次访问时间（用于 LRU tie-break）
    size_t last_access = info.history_.empty() ? 0 : info.history_.back();

    // 第一个合法候选，直接作为初始 best
    if (!found) {
      found = true;
      best_fid = fid;
      best_is_infinite = is_infinite;
      best_last_access = last_access;

      // 若不是 +∞，计算 backward k-distance
      if (!is_infinite) {
        size_t kth_before = info.history_[info.history_.size() - k_];
        best_distance = current_timestamp_ - kth_before;
      }
      continue;
    }

    // 当前是 +∞，而 best 不是 → 当前更优
    if (is_infinite && !best_is_infinite) {
      best_fid = fid;
      best_is_infinite = true;
      best_last_access = last_access;
    } else if (is_infinite && best_is_infinite) {
      if (last_access < best_last_access) {
        best_fid = fid;
        best_last_access = last_access;
      }
    } else if (!is_infinite && !best_is_infinite) {
      size_t kth_before = info.history_[info.history_.size() - k_];
      size_t distance = current_timestamp_ - kth_before;

      // backward k-distance 更大，或者相等但 LRU 更早
      if (distance > best_distance || (distance == best_distance && last_access < best_last_access)) {
        best_fid = fid;
        best_distance = distance;
        best_last_access = last_access;
      }
    }
    // 当前是有限，而 best 是 +∞ → best 更优，什么都不做
  }

  // 理论上不应该发生（curr_size_ > 0 保证至少有一个 evictable）
  if (!found) {
    return false;
  }

  // 真正执行淘汰：删除记录 + 更新可淘汰数量
  records_.erase(best_fid);
  curr_size_--;
  *frame_id = best_fid;
  return true;
}

/**
 * RecordAccess：记录某个 frame 的一次访问
 *
 * 语义：
 * - 每次调用表示该 frame 在“当前时间点”被访问
 * - 时间戳单调递增
 * - 每个 frame 只保留最近 k 次访问记录
 */
void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);

  // frame_id 合法性检查
  BUSTUB_ASSERT(static_cast<size_t>(frame_id) < replacer_size_, "frame_id out of range");

  // 全局时间戳递增
  ++current_timestamp_;

  // 若 frame 不存在，会默认构造 FrameInfo
  auto &info = records_[frame_id];

  // 记录本次访问时间
  info.history_.push_back(current_timestamp_);

  // 只保留最近 k_ 次访问
  while (info.history_.size() > k_) {
    info.history_.pop_front();
  }
}

/**
 * SetEvictable：设置 frame 是否可被淘汰
 *
 * 注意：
 * - 只有 evictable_ == true 的 frame 才能被 Evict
 * - curr_size_ 表示当前“可淘汰 frame 的数量”
 */
void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock<std::mutex> lock(latch_);
  BUSTUB_ASSERT(static_cast<size_t>(frame_id) < replacer_size_, "frame_id out of range");

  // 若不存在，则创建记录（history 为空）
  auto &info = records_[frame_id];

  // 状态未发生变化，直接返回
  if (info.evictable_ == set_evictable) {
    return;
  }

  // 更新 evictable 状态，并同步更新 curr_size_
  info.evictable_ = set_evictable;
  curr_size_ += set_evictable ? 1 : -1;
}

/**
 * Remove：强制移除指定 frame（不是按 LRU-K 规则）
 *
 * 语义：
 * - 仅允许移除 evictable 的 frame
 * - 通常在 page 被删除（DeletePage）时调用
 */
void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);
  BUSTUB_ASSERT(static_cast<size_t>(frame_id) < replacer_size_, "frame_id out of range");

  auto it = records_.find(frame_id);
  if (it == records_.end()) {
    // 不存在则直接返回
    return;
  }

  // 不允许移除不可淘汰的 frame
  BUSTUB_ASSERT(it->second.evictable_, "Remove called on non-evictable frame");

  // 删除记录并更新大小
  records_.erase(it);
  curr_size_--;
}

/**
 * Size：返回当前可被淘汰的 frame 数量
 */
auto LRUKReplacer::Size() -> size_t {
  std::scoped_lock<std::mutex> lock(latch_);
  return curr_size_;
}

}  // namespace bustub
