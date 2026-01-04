//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager_instance.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2021, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager_instance.h"

#include <cstring>  // for memset
#include "common/exception.h"
#include "common/macros.h"

namespace bustub {

BufferPoolManagerInstance::BufferPoolManagerInstance(size_t pool_size, DiskManager *disk_manager, size_t replacer_k,
                                                     LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {
  // we allocate a consecutive memory space for the buffer pool
  pages_ = new Page[pool_size_];
  page_table_ = new ExtendibleHashTable<page_id_t, frame_id_t>(bucket_size_);
  replacer_ = new LRUKReplacer(pool_size_, replacer_k);

  // Initially, every page is in the free list.
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.emplace_back(static_cast<int>(i));
    // mark page metadata as invalid initially
    pages_[i].page_id_ = INVALID_PAGE_ID;  // if Page has public member; if not, SetPageId used below
    // If Page does not expose page_id_ publicly, the header likely sets default INVALID_PAGE_ID already.
  }
}

BufferPoolManagerInstance::~BufferPoolManagerInstance() {
  delete[] pages_;
  delete page_table_;
  delete replacer_;
}

/**
 * 申请一个新的 page（在 buffer pool 中）并返回指向 Page 的指针。
 * 若没有可用 frame（free list 空且 replacer 无法淘汰），返回 nullptr。
 */
auto BufferPoolManagerInstance::NewPgImp(page_id_t *page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);

  // 1. 分配一个新的 page id（逻辑上在磁盘上分配）
  page_id_t new_page_id = AllocatePage();
  // 2. 找到可用的 frame
  frame_id_t frame_id = -1;
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else {
    // 否则使用 replacer 淘汰一个 frame
    frame_id_t victim_frame = -1;
    if (!replacer_->Evict(&victim_frame)) {
      // 没有可淘汰的 frame（全部被 pin），失败
      // 回滚分配的 page id: 按 BusTub 规范，AllocatePage() 简单递增，不需回滚
      return nullptr;
    }
    frame_id = victim_frame;
    // 如果被淘汰的 frame 存在旧页面，需要写回（若 dirty）并从 page table 中删除旧映射
    page_id_t old_page_id = pages_[frame_id].GetPageId();
    if (pages_[frame_id].IsDirty()) {
      disk_manager_->WritePage(old_page_id, pages_[frame_id].GetData());
      pages_[frame_id].is_dirty_ = false;  // 如果 Page 使用 SetDirty / IsDirty 接口，使用那些接口替换
    }
    // 从哈希表中移除旧映射
    page_table_->Remove(old_page_id);
  }

  // 3. 初始化 frame 为新页
  Page *page = &pages_[frame_id];
  // 将页面内存清零（新页的语义）
  std::memset(page->GetData(), 0, BUSTUB_PAGE_SIZE);
  page->page_id_ = new_page_id;  // 若 Page 没有公开成员，请使用 page->SetPageId(new_page_id)
  page->is_dirty_ = false;       // 使用 page->SetDirty(false) 若该接口存在
  // pin 计数设为 1（已被 pin）
  page->pin_count_ = 1;  // 若 Page 有 Pin() 接口，请改用 page->Pin()

  // 4. 在 page table 中插入映射
  page_table_->Insert(new_page_id, frame_id);

  // 5. replacer 更新：记录访问并设置不可淘汰（已 pin）
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  // 返回新分配的 page id 与 Page 指针
  *page_id = new_page_id;
  return page;
}

/**
 * FetchPgImp：获取指定 page_id 的 Page 指针（把它 pin 并返回）。
 * 若没有可用 frame（free list 空且 replacer 无法淘汰），返回 nullptr。
 */
auto BufferPoolManagerInstance::FetchPgImp(page_id_t page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id = -1;
  if (page_table_->Find(page_id, frame_id)) {
    // page 在缓冲池中
    Page *page = &pages_[frame_id];
    page->pin_count_++;  // 若有 Pin() 方法请使用 page->Pin()
    // record access and set non-evictable
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);
    return page;
  }

  // page 不在缓冲池，需要找到一个 frame 来放它
  frame_id_t target_frame = -1;
  if (!free_list_.empty()) {
    target_frame = free_list_.front();
    free_list_.pop_front();
  } else {
    frame_id_t victim = -1;
    if (!replacer_->Evict(&victim)) {
      // 无法找到可淘汰 frame
      return nullptr;
    }
    target_frame = victim;
    // 如果被淘汰 frame 脏，写回磁盘
    page_id_t old_pid = pages_[target_frame].GetPageId();
    if (pages_[target_frame].IsDirty()) {
      disk_manager_->WritePage(old_pid, pages_[target_frame].GetData());
      pages_[target_frame].is_dirty_ = false;
    }
    // 从 page table 删除旧映射
    page_table_->Remove(old_pid);
  }

  // 将目标 frame 填入所请求的 page（从磁盘读）
  Page *page = &pages_[target_frame];
  disk_manager_->ReadPage(page_id, page->GetData());
  page->page_id_ = page_id;
  page->is_dirty_ = false;
  page->pin_count_ = 1;

  // 插入 page_table 映射
  page_table_->Insert(page_id, target_frame);

  // replacer 更新
  replacer_->RecordAccess(target_frame);
  replacer_->SetEvictable(target_frame, false);

  return page;
}

/**
 * UnpinPgImp：解除 pin（减少 pin_count），并根据 is_dirty 更新 dirty 标志。
 * 当 pin_count 减为 0 时，将 frame 标记为可淘汰。
 */
auto BufferPoolManagerInstance::UnpinPgImp(page_id_t page_id, bool is_dirty) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id = -1;
  if (!page_table_->Find(page_id, frame_id)) {
    // page 不在缓冲池
    return false;
  }

  Page *page = &pages_[frame_id];
  if (page->pin_count_ <= 0) {
    return false;
  }

  // 标记脏位
  if (is_dirty) {
    page->is_dirty_ = true;
  }

  // 解除 pin
  page->pin_count_--;  // 若 Page 提供 Unpin / Pin API，请使用之
  if (page->pin_count_ == 0) {
    // 变为可淘汰，通知 replacer
    replacer_->SetEvictable(frame_id, true);
  }

  return true;
}

/**
 * FlushPgImp：将指定 page 写回磁盘（无论是否被 pin）。
 */
auto BufferPoolManagerInstance::FlushPgImp(page_id_t page_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id = -1;
  if (!page_table_->Find(page_id, frame_id)) {
    return false;
  }

  Page *page = &pages_[frame_id];
  disk_manager_->WritePage(page_id, page->GetData());
  page->is_dirty_ = false;
  return true;
}

/**
 * FlushAllPgsImp：将所有在缓冲池中的页面全部写回磁盘。
 */
void BufferPoolManagerInstance::FlushAllPgsImp() {
  std::scoped_lock<std::mutex> lock(latch_);
  for (size_t i = 0; i < pool_size_; ++i) {
    if (pages_[i].GetPageId() != INVALID_PAGE_ID) {
      disk_manager_->WritePage(pages_[i].GetPageId(), pages_[i].GetData());
      pages_[i].is_dirty_ = false;
    }
  }
}

/**
 * DeletePgImp：删除指定 page（从磁盘逻辑上回收）。
 * 如果 page 在缓冲池中且 pin_count > 0，返回 false（不能删除正在使用的页）。
 * 否则移除页面（并且将 frame 复位、放回 free_list），最后调用 DeallocatePage。
 */
auto BufferPoolManagerInstance::DeletePgImp(page_id_t page_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t frame_id = -1;
  if (!page_table_->Find(page_id, frame_id)) {
    // page 不在缓冲池，直接在磁盘上 deallocate（no-op）
    DeallocatePage(page_id);
    return true;
  }

  Page *page = &pages_[frame_id];
  if (page->pin_count_ > 0) {
    // 正在被引用，不能删除
    return false;
  }

  // 若脏则写回（可选）
  if (page->IsDirty()) {
    disk_manager_->WritePage(page_id, page->GetData());
    page->is_dirty_ = false;
  }

  // 从 page_table 中移除映射
  page_table_->Remove(page_id);

  // 将 frame 复位为未使用
  page->page_id_ = INVALID_PAGE_ID;
  page->pin_count_ = 0;
  page->is_dirty_ = false;
  // 添加到 free list
  free_list_.push_back(frame_id);

  // 确保 replacer 不再追踪该 frame：先设为可淘汰再移除（若 replacer 中没有记录，SetEvictable 会创建记录，Remove
  // 会移除）
  replacer_->SetEvictable(frame_id, true);
  replacer_->Remove(frame_id);

  // 在磁盘上回收 page id
  DeallocatePage(page_id);
  return true;
}

auto BufferPoolManagerInstance::AllocatePage() -> page_id_t { return next_page_id_++; }

}  // namespace bustub
