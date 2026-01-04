//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.cpp
//
// Identification: src/container/hash/extendible_hash_table.cpp
//
// Copyright (c) 2022, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstdlib>
#include <functional>
#include <list>
#include <utility>

#include "container/hash/extendible_hash_table.h"
#include "storage/page/page.h"

namespace bustub {

template <typename K, typename V>
ExtendibleHashTable<K, V>::ExtendibleHashTable(size_t bucket_size)
    : global_depth_(0), bucket_size_(bucket_size), num_buckets_(1) {
  // 初始化目录：global_depth_ = 0 -> 目录长度 = 1
  dir_.resize(1);
  dir_[0] = std::make_shared<Bucket>(bucket_size_, 0);
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::IndexOf(const K &key) -> size_t {
  // 注意：当 global_depth_ == 0 时 mask == 0，返回 0
  if (global_depth_ == 0) {
    return 0;
  }
  size_t mask = (static_cast<size_t>(1) << global_depth_) - 1;
  return std::hash<K>()(key) & mask;
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetGlobalDepth() const -> int {
  std::scoped_lock<std::mutex> lock(latch_);
  return GetGlobalDepthInternal();
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetGlobalDepthInternal() const -> int {
  return global_depth_;
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetLocalDepth(int dir_index) const -> int {
  std::scoped_lock<std::mutex> lock(latch_);
  return GetLocalDepthInternal(dir_index);
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetLocalDepthInternal(int dir_index) const -> int {
  if (dir_index < 0 || static_cast<size_t>(dir_index) >= dir_.size()) {
    return -1;
  }
  return dir_[dir_index]->GetDepth();
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetNumBuckets() const -> int {
  std::scoped_lock<std::mutex> lock(latch_);
  return GetNumBucketsInternal();
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::GetNumBucketsInternal() const -> int {
  return num_buckets_;
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::Find(const K &key, V &value) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);
  size_t idx = IndexOf(key);
  auto bucket = dir_[idx];
  return bucket->Find(key, value);
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::Remove(const K &key) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);
  size_t idx = IndexOf(key);
  auto bucket = dir_[idx];
  return bucket->Remove(key);
}

template <typename K, typename V>
void ExtendibleHashTable<K, V>::Insert(const K &key, const V &value) {
  std::scoped_lock<std::mutex> lock(latch_);
  while (true) {
    size_t idx = IndexOf(key);
    auto bucket = dir_[idx];
    // 如果能直接插入（包括覆盖已存在 key），就返回
    if (bucket->Insert(key, value)) {
      return;
    }

    // 否则，桶已满且 key 不存在 —— 需要分裂
    // 如果该 bucket 的局部深度等于全局深度，先扩展目录（double）
    if (bucket->GetDepth() == global_depth_) {
      size_t old_size = dir_.size();
      dir_.resize(old_size * 2);
      for (size_t i = 0; i < old_size; ++i) {
        dir_[i + old_size] = dir_[i];
      }
      global_depth_++;
    }

    // 进行 bucket 分裂
    // 增加局部深度，创建新桶，并把部分目录指向新桶
    bucket->IncrementDepth();
    int new_depth = bucket->GetDepth();
    auto new_bucket = std::make_shared<Bucket>(bucket_size_, new_depth);
    num_buckets_++;

    // 根据 new_depth 的最高位（即 bit = new_depth - 1）来区分目录索引的归属
    int bit = new_depth - 1;
    for (size_t i = 0; i < dir_.size(); ++i) {
      if (dir_[i] == bucket) {
        if (((i >> bit) & 1) == 1) {
          dir_[i] = new_bucket;
        } else {
          // 保持指向原 bucket
        }
      }
    }
    // 重新分配原 bucket 的所有 kv 到目录现在指向的桶里
    std::list<std::pair<K, V>> old_items = std::move(bucket->GetItems());
    // bucket->GetItems() 已经被移动，确保原桶的 list 为空
    bucket->GetItems().clear();

    for (auto &kv : old_items) {
      size_t new_idx = IndexOf(kv.first);
      dir_[new_idx]->GetItems().push_back(std::move(kv));
    }

    // 分裂并重分配完成，回到循环重试插入（此时目录/桶结构已更新）
  }
}

//===--------------------------------------------------------------------===//
// Bucket
//===--------------------------------------------------------------------===//
template <typename K, typename V>
ExtendibleHashTable<K, V>::Bucket::Bucket(size_t array_size, int depth) : size_(array_size), depth_(depth) {}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::Bucket::Find(const K &key, V &value) -> bool {
  for (auto &p : list_) {
    if (p.first == key) {
      value = p.second;
      return true;
    }
  }
  return false;
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::Bucket::Remove(const K &key) -> bool {
  for (auto it = list_.begin(); it != list_.end(); ++it) {
    if (it->first == key) {
      list_.erase(it);
      return true;
    }
  }
  return false;
}

template <typename K, typename V>
auto ExtendibleHashTable<K, V>::Bucket::Insert(const K &key, const V &value) -> bool {
  // 如果已经存在，更新并返回 true
  for (auto &p : list_) {
    if (p.first == key) {
      p.second = value;
      return true;
    }
  }
  // 否则如果已满，返回 false（上层负责 split）
  if (IsFull()) {
    return false;
  }
  // 插入新项
  list_.emplace_back(key, value);
  return true;
}

template class ExtendibleHashTable<page_id_t, Page *>;
template class ExtendibleHashTable<Page *, std::list<Page *>::iterator>;
template class ExtendibleHashTable<int, int>;
// test purpose
template class ExtendibleHashTable<int, std::string>;
template class ExtendibleHashTable<int, std::list<int>::iterator>;

}  // namespace bustub
