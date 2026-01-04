#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/exception.h"
#include "common/rwlatch.h"
namespace bustub {
class TrieNode {
 protected:
  char key_char_;
  bool is_end_{false};
  std::unordered_map<char, std::unique_ptr<TrieNode>> children_;

 public:
  explicit TrieNode(char key_char) {
    key_char_ = key_char;
    is_end_ = false;
  }
  TrieNode(TrieNode &&other_trie_node) noexcept {
    key_char_ = other_trie_node.key_char_;
    is_end_ = other_trie_node.is_end_;
    children_ = std::move(other_trie_node.children_);
  }

  /**
   * @brief Destroy the TrieNode object.
   */
  virtual ~TrieNode() = default;

  bool HasChild(char key_char) const { return children_.find(key_char) != children_.end(); }

  bool HasChildren() const { return !children_.empty(); }

  bool IsEndNode() const { return is_end_; }

  char GetKeyChar() const { return key_char_; }

  std::unique_ptr<TrieNode> *InsertChildNode(char key_char, std::unique_ptr<TrieNode> &&child) {
    if (children_.find(key_char) != children_.end()) {
      return nullptr;
    }
    if (child->GetKeyChar() != key_char) {
      return nullptr;
    }
    children_[key_char] = std::move(child);
    return &children_[key_char];
  }

  std::unique_ptr<TrieNode> *GetChildNode(char key_char) {
    if (children_.find(key_char) == children_.end()) {
      return nullptr;
    }
    return &children_[key_char];
  }

  void RemoveChildNode(char key_char) {
    if (children_.find(key_char) == children_.end()) {
      return;
    }
    children_.erase(key_char);
  }

  void SetEndNode(bool is_end) { is_end_ = is_end; }
};

template <typename T>
class TrieNodeWithValue : public TrieNode {
 private:
  /* Value held by this trie node. */
  T value_;

 public:
  TrieNodeWithValue(TrieNode &&trieNode, T value) : TrieNode(std::move(trieNode)), value_(value) { is_end_ = true; }

  TrieNodeWithValue(char key_char, T value) : TrieNode(key_char), value_(value) { is_end_ = true; }

  /**
   * @brief Destroy the Trie Node With Value object
   */
  ~TrieNodeWithValue() override = default;

  T GetValue() const { return value_; }
};

class Trie {
 private:
  std::unique_ptr<TrieNode> root_;
  ReaderWriterLatch latch_;

 public:
  Trie() : root_(std::make_unique<TrieNode>('\0')) {}

  template <typename T>
  bool Insert(const std::string &key, T value) {
    if (key.empty()) {
      return false;
    }
    latch_.WLock();
    TrieNode *current = root_.get();
    for (size_t i = 0; i < key.size(); ++i) {
      char c = key[i];
      if (!current->HasChild(c)) {
        if (i == key.size() - 1) {
          auto new_node = std::make_unique<TrieNodeWithValue<T>>(c, value);
          current->InsertChildNode(c, std::move(new_node));
        } else {
          auto new_node = std::make_unique<TrieNode>(c);
          current->InsertChildNode(c, std::move(new_node));
        }
      } else {
        if (i == key.size() - 1) {
          if (current->GetChildNode(c)->get()->IsEndNode()) {
            latch_.WUnlock();
            return false;
          }
          auto old_node = std::move(*current->GetChildNode(c));
          auto new_node = std::make_unique<TrieNodeWithValue<T>>(std::move(*old_node), value);
          current->RemoveChildNode(c);
          current->InsertChildNode(c, std::move(new_node));
        }
      }
      current = current->GetChildNode(c)->get();
    }
    latch_.WUnlock();
    return true;
  }

  bool Remove(const std::string &key) {
    if (key.empty()) {
      return false;
    }
    latch_.WLock();
    std::vector<TrieNode *> nodes;
    TrieNode *current = root_.get();
    nodes.push_back(current);
    for (char c : key) {
      if (!current->HasChild(c)) {
        latch_.WUnlock();
        return false;
      }
      current = current->GetChildNode(c)->get();
      nodes.push_back(current);
    }
    auto it = nodes.back();
    if (!it->IsEndNode()) {
      latch_.WUnlock();
      return false;
    }
    it->SetEndNode(false);
    if (it->HasChildren()) {
      latch_.WUnlock();
      return true;
    }
    TrieNode *pa = nodes[key.size() - 1];
    pa->RemoveChildNode(it->GetKeyChar());
    for (int i = key.size() - 1; i >= 1; --i) {
      TrieNode *node = nodes[i];
      TrieNode *parent = nodes[i - 1];
      if (node->HasChildren() || node->IsEndNode()) {
        break;
      }
      parent->RemoveChildNode(node->GetKeyChar());
    }
    latch_.WUnlock();
    return true;
  }

  template <typename T>
  T GetValue(const std::string &key, bool *success) {
    if (key.empty()) {
      *success = false;
      return T();
    }
    latch_.RLock();
    TrieNode *current = root_.get();
    for (char c : key) {
      if (!current->HasChild(c)) {
        latch_.RUnlock();
        *success = false;
        return T();
      }
      current = current->GetChildNode(c)->get();
    }
    auto terminal_node = dynamic_cast<TrieNodeWithValue<T> *>(current);
    if (terminal_node == nullptr) {
      latch_.RUnlock();
      *success = false;
      return T();
    }
    T value = terminal_node->GetValue();
    latch_.RUnlock();
    *success = true;
    return value;
  }
};
}  // namespace bustub
