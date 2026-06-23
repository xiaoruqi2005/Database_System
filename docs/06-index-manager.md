# 06 索引层

> 相关源文件:`src/index/ix_defs.h`、`src/index/ix_index_handle.{h,cpp}`、`src/index/ix_manager.h`、`src/index/ix_scan.{h,cpp}`

## 一句话职责

索引层在存储层之上实现 **B+ 树索引**,支持按键查找、范围扫描和键值插入/删除。每个索引对应一个 `.db` 文件,内部以 B+ 树的叶子节点和内部节点组织。

---

## B+ 树节点结构

### 内部节点(`IxPageHandle` of internal page)

```
+--------------------------------------------------+
| IxPageHdr                                        |
|   parent_page_no                                  |
|   num_key                                         |
|   is_leaf = false                                 |
|   next_free_page_no                               |
+--------------------------------------------------+
| Keys:   [key_0] [key_1] ... [key_{n-1}]          |
+--------------------------------------------------+
| Child pointers:                                   |
|   [child_0] [child_1] ... [child_n]              |
|   (child_i points to subtree for keys < key_i)   |
+--------------------------------------------------+
```

### 叶子节点(`IxPageHandle` of leaf page)

```
+--------------------------------------------------+
| IxPageHdr                                        |
|   parent_page_no                                  |
|   num_key                                         |
|   is_leaf = true                                  |
|   next_free_page_no  (= next leaf for scan)      |
+--------------------------------------------------+
| Keys:   [key_0] [key_1] ... [key_{n-1}]          |
+--------------------------------------------------+
| Values: [rid_0] [rid_1] ... [rid_{n-1}]          |
|   (each rid = {page_no, slot_no} of a record)    |
+--------------------------------------------------+
```

叶子节点通过 `next_free_page_no` 串成有序链表,支持范围扫描。

---

## IxIndexHandle — B+ 树句柄

### 核心方法

| 方法 | 签名 | 行为 |
|---|---|---|
| `insert_entry` | `bool insert_entry(const char *key, const Rid &value, Context *)` | 插入键值对;若键已存在则返回 false |
| `delete_entry` | `bool delete_entry(const char *key, Context *)` | 删除键值对;可能触发节点合并/借位 |
| `lower_bound` | `Iid lower_bound(const char *key)` | 返回第一个 >= key 的叶子项位置 |
| `upper_bound` | `Iid upper_bound(const char *key)` | 返回第一个 > key 的叶子项位置 |
| `find_leaf_page` | `Page* find_leaf_page(const char *key, Operation)` | 从根遍历到 key 对应的叶子页 |

### 插入流程

```
insert_entry(key, value):
  1. find_leaf_page(key) -> leaf page L
  2. if key already exists in L: return false
  3. insert (key, value) into L in sorted order
  4. if L is not full: return true
  5. if L is full (overflow):
     5a. split L into L and L'
     5b. copy up middle key to parent
     5c. if parent overflows: recursively split upward
     5d. if root splits: create new root
```

### 删除流程

```
delete_entry(key):
  1. find_leaf_page(key) -> leaf page L
  2. remove key from L
  3. if L still >= half full: return true
  4. if L underflows:
     4a. try to borrow from sibling (redistribution)
     4b. if cannot borrow: merge with sibling
     4c. remove separator key from parent
     4d. if parent underflows: recursively handle
     4e. if root becomes empty: update root
```

---

## IxManager — 索引管理器

```cpp
class IxManager {
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
public:
    int create_index(const std::string &filename, ColType col_type, int col_len);
    std::unique_ptr<IxIndexHandle> open_index(const std::string &filename);
    void destroy_index(const std::string &filename);
};
```

索引文件名约定:通常以 `表名.列名.db` 命名。`create_index` 创建文件并初始化 B+ 树的根节点(一个空叶子)。

---

## IxScan — 索引范围扫描

```cpp
class IxScan : public RecScan {
    IxIndexHandle *ih_;
    Iid iid_;            // current position in leaf page
    Iid end_;            // exclusive end position
public:
    void next() override;          // advance to next key in leaf chain
    bool is_end() const override;  // reached end_?
    Rid rid() const override;      // return Rid stored at current position
};
```

`IxScan` 由 `lower_bound`/`upper_bound` 界定范围,沿着叶子节点链表顺序遍历,每次 `next()` 前进到下一个键,`rid()` 返回该项存储的记录定位符。

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `IxIndexHandle`(insert/delete/lookup/split/merge) | ✅ 已实现 | lab1 完成 |
| `IxManager`(create/open/destroy) | ✅ 已实现 | lab1 完成 |
| `IxScan` | ✅ 已实现 | lab1 完成 |

> 上一篇:[05-record-manager](05-record-manager.md)
> 下一篇:[07-system-manager](07-system-manager.md) — 系统管理层。
