# 04 存储层

> 相关源文件:`src/storage/page.h`、`src/storage/disk_manager.{h,cpp}`、`src/storage/buffer_pool_manager.{h,cpp}`、`src/replacer/replacer.h`、`src/replacer/lru_replacer.{h,cpp}`

## 一句话职责

存储层是 RMDB 的最底层,负责把 4KB 的 `Page` 在磁盘文件与内存缓冲池之间搬运,并通过 LRU 替换策略管理有限的缓冲池帧。

---

## Page — 内存中的页帧

```cpp
class Page {
    friend class BufferPoolManager;
    int id_;               // page id (or file fd encoded)
    page_id_t page_id_ = INVALID_PAGE_ID;
    int pin_count_ = 0;
    bool is_dirty_ = false;
    lsn_t lsn_ = INVALID_LSN;
    char data_[PAGE_SIZE] = {};
    std::shared_mutex latch_;
    std::mutex rwlatch_;
public:
    char *GetData();              // pointer to data_[]
    page_id_t GetPageId() const;
    int GetPinCount();
    lsn_t GetLsn();
    bool IsDirty();
    void WLatch(); / void WUnlatch();   // write latch
    void RLatch(); / void RUnlatch();   // read latch
};
```

### 页内布局

```
+--------------------------------------------------+
| Page header (metadata)                           |
|   id_ | page_id_ | pin_count_ | is_dirty_ | lsn_ |
+--------------------------------------------------+
| data_[PAGE_SIZE]  (4096 bytes)                   |
|   ... actual page content ...                    |
+--------------------------------------------------+
```

- `data_[PAGE_SIZE]` 是实际存放文件内容的区域,上层模块(记录层、索引层)把各自的结构 cast 到这片内存。
- `pin_count_` 表示该页被多少上层操作引用,>0 时不能被淘汰。
- `is_dirty_` 表示该页被修改过,淘汰前需写回磁盘。
- `lsn_` 用于恢复层(WAL)。

---

## DiskManager — 磁盘 I/O

### 核心字段

```cpp
class DiskManager {
    int fd_ = -1;                           // current db directory fd
    std::unordered_map<std::string, int> path2fd_;   // file path -> fd
    std::unordered_map<int, std::string> fd2path_;   // fd -> file path
    static std::atomic<int> global_fd;
};
```

### 核心方法

| 方法 | 行为 |
|---|---|
| `ReadPage(fd, page_no, page_data)` | 从文件 `fd` 偏移 `page_no * PAGE_SIZE` 读取 4096 字节到 `page_data` |
| `WritePage(fd, page_no, page_data)` | 将 4096 字节写回文件 `fd` 的对应偏移 |
| `AllocatePage(fd)` | 通过 `lseek` 扩展文件,返回新页号 |
| `DeallocatePage(fd, page_no)` | 释放页(标记为空闲) |
| `IsFileTemp(path)` | 判断文件是否存在 |
| `CreateFile(path)` | 创建新文件 |
| `DestroyFile(path)` | 删除文件 |
| `OpenFile(path)` | 打开文件,记录 fd 映射 |
| `CloseFile(fd)` | 关闭文件 |
| `ShutDown()` | 关闭所有打开的文件 |

### 文件布局

每个文件由连续的 `PAGE_SIZE` 大小的页组成,页号从 0 开始:

```
File: table1.db
+---------+---------+---------+---------+
| Page 0  | Page 1  | Page 2  | Page 3  | ...
| (header)| (data)  | (data)  | (data)  |
+---------+---------+---------+---------+
  offset    PAGE_SIZE 2*PAGE_SIZE ...
  0
```

`ReadPage/WritePage` 通过 `pread/pwrite` 或 `lseek+read/write` 定位到 `page_no * PAGE_SIZE`。

---

## Replacer — 替换器接口

```cpp
class Replacer {
public:
    virtual bool Victim(frame_id_t *frame_id) = 0;  // evict a frame
    virtual void Pin(frame_id_t frame_id) = 0;       // mark frame as pinned
    virtual void Unpin(frame_id_t frame_id) = 0;     // mark frame as unpinned
    virtual size_t Size() = 0;                        // # of unpinned frames
};
```

### LRUReplacer 实现

```cpp
class LRUReplacer : public Replacer {
    std::list<frame_id_t> lru_list_;                          // front = most recent
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_map_;
    size_t max_size_ = BUFFER_POOL_SIZE;
    std::mutex latch_;
};
```

- `Victim`:取 `lru_list_` 尾部(最久未使用)的帧号,从结构中移除。
- `Pin`:从 LRU 结构中删除该帧(表示正在使用)。
- `Unpin`:把该帧插入到 LRU 首部(最近使用过)。

```
LRU eviction order (Victim picks tail):
  head (recently used)            tail (evict first)
   |                                |
   v                                v
  [frame 5] -> [frame 2] -> [frame 8] -> ...
```

---

## BufferPoolManager — 缓冲池管理器

### 核心字段

```cpp
class BufferPoolManager {
    DiskManager *disk_mgr_;
    Replacer *replacer_;                    // LRU replacer
    std::unordered_map<PageId, frame_id_t, PageIdHash> page_table_;  // PageId -> frame
    std::list<frame_id_t> free_list_;       // unused frames
    Page *pages_;                           // array of BUFFER_POOL_SIZE Page objects
    std::mutex latch_;
};
```

### 核心方法

| 方法 | 签名 | 行为 |
|---|---|---|
| `FetchPage` | `Page* fetch_page(PageId page_id)` | 若页在缓冲池,返回它并 `pin_count++`;否则找空闲帧或淘汰一帧,从磁盘读入 |
| `UnpinPage` | `bool unpin_page(PageId page_id, bool is_dirty)` | `pin_count--`,标记 dirty |
| `NewPage` | `Page* new_page(PageId* page_id)` | 分配新页(扩展文件),返回 `Page*` |
| `DeletePage` | `bool delete_page(PageId page_id)` | 仅当 `pin_count==0` 时删除页 |
| `FlushPage` | `bool flush_page(PageId page_id)` | 若 dirty 则写回磁盘 |
| `FlushAllPages` | `void flush_all_pages(int fd)` | 刷新某文件的所有 dirty 页 |

### FetchPage 工作流程

```
FetchPage(page_id):
  1. lookup page_id in page_table_
     |-- found: pin_count++, replacer.Pin(frame), return pages_[frame]
     |-- not found: goto step 2
  2. find a free frame:
     |-- free_list_ not empty: take a frame from it
     |-- free_list_ empty:
         2a. replacer.Victim(&frame)
         2b. if pages_[frame].is_dirty_ -> WritePage to disk
         2c. remove old PageId from page_table_
  3. ReadPage(disk, page_id) -> pages_[frame].data_
  4. pages_[frame].page_id_ = page_id; pin_count_ = 1; is_dirty_ = false
  5. page_table_[page_id] = frame
  6. return &pages_[frame]
```

### UnpinPage 工作流程

```
UnpinPage(page_id, is_dirty):
  1. find frame in page_table_
  2. pin_count--
  3. if is_dirty: mark page is_dirty_ = true
  4. if pin_count == 0: replacer.Unpin(frame)  // eligible for eviction
```

---

## 数据流总览

```
Upper layer (RmFileHandle / IxIndexHandle)
       |
       | fetch_page(PageId) / new_page / unpin_page
       v
BufferPoolManager -------> pages_[] (in-memory cache)
       |                        |
       | (cache miss)           | (dirty eviction)
       v                        v
DiskManager.ReadPage    DiskManager.WritePage
       |
       v
  .db file on disk
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `Page` | ✅ 已实现 | lab1 完成 |
| `DiskManager` | ✅ 已实现 | lab1 完成 |
| `Replacer` / `LRUReplacer` | ✅ 已实现 | lab1 完成 |
| `BufferPoolManager` | ✅ 已实现 | lab1 完成 |

> 上一篇:[03-common-infrastructure](03-common-infrastructure.md)
> 下一篇:[05-record-manager](05-record-manager.md) — 记录管理层。