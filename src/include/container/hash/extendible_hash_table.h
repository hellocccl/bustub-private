//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_hash_table.h
//
// Identification: src/include/container/hash/extendible_hash_table.h
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
/**
 * extendible_hash_table.h
 *
 * 使用可扩展哈希（extendible hashing）实现的内存哈希表
 */

#pragma once

#include <list>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>

#include "container/hash/hash_table.h"

namespace bustub {

/**
 * ExtendibleHashTable 使用可扩展哈希算法实现哈希表。
 * @tparam K 键类型
 * @tparam V 值类型
 */
template <typename K, typename V>
class ExtendibleHashTable : public HashTable<K, V> {
 public:
  /**
   *
   * TODO(P1): Add implementation
   *
   * @brief Create a new ExtendibleHashTable.
   * @param bucket_size: fixed size for each bucket
   */
  explicit ExtendibleHashTable(size_t bucket_size);

  /**
   * @brief 获取目录的全局深度。
   * @return 目录的全局深度。
   */
  auto GetGlobalDepth() const -> int;

  /**
   * @brief 获取给定目录索引指向的桶的局部深度。
   * @param dir_index 目录中的索引。
   * @return 桶的局部深度。
   */
  auto GetLocalDepth(int dir_index) const -> int;

  /**
   * @brief 获取目录中桶的数量。
   * @return 目录中桶的数量。
   */
  auto GetNumBuckets() const -> int;

  /**
   *
   * TODO(P1): Add implementation
   *
   * @brief 查找与给定键关联的值。
   *
   * 使用 IndexOf(key) 找到键哈希到的目录索引。
   *
   * @param key 要查找的键。
   * @param[out] value 与键关联的值。
   * @return 找到返回 true，否则返回 false。
   */
  auto Find(const K &key, V &value) -> bool override;

  /**
   *
   * TODO(P1): Add implementation
   *
   * @brief 将给定的键值对插入哈希表。
   * 如果键已存在，应更新其值。
   * 如果桶已满且无法插入，重试前执行以下步骤：
   *    1. 如果该桶的局部深度等于全局深度，
   *       则增加全局深度并将目录大小加倍。
   *    2. 增加该桶的局部深度。
   *    3. 拆分该桶并重新分配目录指针以及桶内的键值对。
   *
   * @param key 要插入的键。
   * @param value 要插入的值。
   */
  void Insert(const K &key, const V &value) override;

  /**
   *
   * TODO(P1): Add implementation
   *
   * @brief 根据键在哈希表中删除对应的键值对。
   * 本项目不要求执行收缩和合并操作。
   * @param key 要删除的键。
   * @return 如果键存在返回 true，否则返回 false。
   */
  auto Remove(const K &key) -> bool override;

  /**
   * Bucket class for each hash table bucket that the directory points to.
   */
  class Bucket {
   public:
    explicit Bucket(size_t size, int depth = 0);

    /** @brief 检查桶是否已满。 */
    inline auto IsFull() const -> bool { return list_.size() == size_; }

    /** @brief 获取桶的局部深度。 */
    inline auto GetDepth() const -> int { return depth_; }

    /** @brief 增加桶的局部深度。 */
    inline void IncrementDepth() { depth_++; }

    inline auto GetItems() -> std::list<std::pair<K, V>> & { return list_; }

    /**
     *
     * TODO(P1): Add implementation
     *
     * @brief 在桶中查找与给定键关联的值。
     * @param key 要查找的键。
     * @param[out] value 与键关联的值。
     * @return 找到返回 true，否则返回 false。
     */
    auto Find(const K &key, V &value) -> bool;

    /**
     *
     * TODO(P1): Add implementation
     *
     * @brief 根据键在桶中删除对应的键值对。
     * @param key 要删除的键。
     * @return 如果键存在返回 true，否则返回 false。
     */
    auto Remove(const K &key) -> bool;

    /**
     *
     * TODO(P1): Add implementation
     *
     * @brief 将给定的键值对插入到桶中。
     *      1. 如果键已存在，则应更新其值。
     *      2. 如果桶已满，则不做任何操作并返回 false。
     * @param key 要插入的键。
     * @param value 要插入的值。
     * @return 插入成功返回 true，否则返回 false。
     */
    auto Insert(const K &key, const V &value) -> bool;

   private:
    // TODO(student): You may add additional private members and helper functions
    size_t size_;
    int depth_;
    std::list<std::pair<K, V>> list_;
  };

 private:
  // TODO(student): You may add additional private members and helper functions and remove the ones
  // you don't need.

  int global_depth_;    // The global depth of the directory
  size_t bucket_size_;  // The size of a bucket
  int num_buckets_;     // The number of buckets in the hash table
  mutable std::mutex latch_;
  std::vector<std::shared_ptr<Bucket>> dir_;  // The directory of the hash table

  // The following functions are completely optional, you can delete them if you have your own ideas.

  /**
   * @brief Redistribute the kv pairs in a full bucket.
   * @param bucket The bucket to be redistributed.
   */
  auto RedistributeBucket(std::shared_ptr<Bucket> bucket) -> void;

  /*****************************************************************
   * Must acquire latch_ first before calling the below functions. *
   *****************************************************************/

  /**
   * @brief For the given key, return the entry index in the directory where the key hashes to.
   * @param key The key to be hashed.
   * @return The entry index in the directory.
   */
  auto IndexOf(const K &key) -> size_t;

  auto GetGlobalDepthInternal() const -> int;
  auto GetLocalDepthInternal(int dir_index) const -> int;
  auto GetNumBucketsInternal() const -> int;
};

}  // namespace bustub
