# 05 记录管理层

> 相关源文件:`src/record/rm_defs.h`、`src/record/rm_file_handle.{h,cpp}`、`src/record/rm_manager.h`、`src/record/rm_scan.{h,cpp}`、`src/record/bitmap.h`

## 一句话职责

记录管理层在存储层的页之上,实现**定长记录**(tuple)的存储、插入、删除、更新和扫描。每张表对应一个 `.db` 文件,文件内部按页组织,每页存放多条记录。

---

## RmFileHandle — 记录文件句柄

### 文件头页(`RmFileHandle` 的第一个页)

```cpp
struct RmFileHdr {
    int record_size;        // 每条记录的字节数(定长)
    int num_pages;          // 文件总页数(含头页)
    int num_records_per_page; // 每页最多容纳的记录数
    int first_free_page_no; // 第一个有空闲槽的页号(空闲链表头)
    Bitmap bitmap;          // (实际存在页内)
};
```

### 数据页布局(`RmPageHandle`)

```
+--------------------------------------------------+
| RmPageHdr                                        |
|   next_free_page_no   (空闲链表下一个)            |
+--------------------------------------------------+
| Bitmap  (num_records_per_page bits)              |
|   bit i = 1 表示 slot i 已被占用                  |
+--------------------------------------------------+
| Record slots (each record_size bytes)            |
|   [slot 0] [slot 1] [slot 2] ... [slot N-1]      |
+--------------------------------------------------+
```

每页可容纳的记录数:
```
num_records_per_page = (PAGE_SIZE - sizeof(RmPageHdr)) / (record_size + 1/8)
```
(每个槽额外需要 1 bit 的 bitmap 空间)

### 核心方法

| 方法 | 签名 | 行为 |
|---|---|---|
| `insert_record` | `Rid insert_record(char *buf, Context *)` | 在空闲槽插入记录,返回 Rid;若当前空闲页已满,分配新页 |
| `delete_record` | `void delete_record(Rid rid, Context *)` | 清除 bitmap 位,更新空闲链表 |
| `update_record` | `void update_record(Rid rid, char *buf, Context *)` | 覆写指定槽 |
| `get_record` | `std::unique_ptr<RmRecord> get_record(Rid rid, Context *)` | 读取指定槽的记录数据 |

### 空闲页链表

```
FileHdr.first_free_page_no -> Page 2 -> Page 5 -> NULL
                                  |
                            (each page's next_free_page_no
                             points to next free page)
```

插入时:从 `first_free_page_no` 指向的页找空闲槽;若该页满了,沿链表前进;若链表耗尽,分配新页并加入链表头。

删除时:若某页从"满"变为"有空闲",把它插入链表头。

---

## RmManager — 记录文件管理器

```cpp
class RmManager {
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
public:
    int create_file(const std::string &filename, int record_size);
    std::unique_ptr<RmFileHandle> open_file(const std::string &filename);
    void destroy_file(const std::string &filename);
};
```

- `create_file`:创建文件,写入文件头页(record_size 等)。
- `open_file`:打开文件,读取文件头页,构造 `RmFileHandle`。
- `destroy_file`:关闭并删除文件。

---

## RmScan — 记录扫描器

```cpp
class RmScan : public RecScan {
    RmFileHandle *file_handle_;
    Rid rid_;           // current record position
public:
    void next() override;          // skip to next occupied slot
    bool is_end() const override;  // reached end of file?
    Rid rid() const override;      // return current Rid
};
```

`RmScan` 遍历文件的所有数据页,跳过 bitmap 中未占用的槽,只返回实际存在的记录。

```
Scan order:
  Page 1 slot 0 -> slot 1 -> ... -> slot N
  -> Page 2 slot 0 -> slot 1 -> ...
  -> ... until num_pages reached
```

---

## bitmap.h — 位图工具

```cpp
class Bitmap {
    int num_bits;
    char *bits;          // underlying byte array
public:
    bool is_set(int bit);       // check if bit i is 1
    void set(int bit);          // set bit i to 1
    void reset(int bit);        // set bit i to 0
    int find_first_zero();      // find first 0 bit (free slot)
};
```

`Bitmap` 直接操作页内的一段连续字节,用于管理槽的占用状态。

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `RmFileHandle`(insert/delete/update/get) | ✅ 已实现 | lab1 完成 |
| `RmManager`(create/open/destroy) | ✅ 已实现 | lab1 完成 |
| `RmScan` | ✅ 已实现 | lab1 完成 |
| `Bitmap` | ✅ 已实现 | lab1 完成 |

> 上一篇:[04-storage-layer](04-storage-layer.md)
> 下一篇:[06-index-manager](06-index-manager.md) — 索引层。
