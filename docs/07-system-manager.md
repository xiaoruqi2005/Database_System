# 07 系统管理层

> 相关源文件:`src/system/sm_defs.h`、`src/system/sm_meta.h`、`src/system/sm_manager.{h,cpp}`

## 一句话职责

系统管理层负责**数据库、表、索引的元数据管理**,处理 DDL 语句(CREATE/DROP/DESC/SHOW),并维护元数据与磁盘文件的一致性。

---

## 元数据体系(`sm_meta.h`)

### `ColMeta` — 列元数据

```cpp
struct ColMeta {
    std::string name;        // 列名
    ColType type;            // 列类型 (INT/FLOAT/STRING)
    int len;                 // 列长度(字节数)
    int offset;              // 在记录中的字节偏移
    bool index;              // 是否建有索引
};
```

### `TabMeta` — 表元数据

```cpp
struct TabMeta {
    std::string name;                         // 表名
    std::vector<ColMeta> cols;                // 列定义(按建表顺序)
    std::unordered_map<std::string, int> col_map;  // 列名 -> 在 cols 中的下标
    int record_size;                          // 一条记录的总字节数
};
```

### `DbMeta` — 数据库元数据

```cpp
struct DbMeta {
    std::string name;                                  // 数据库名
    std::unordered_map<std::string, TabMeta> tabs_;    // 表名 -> 表元数据
};
```

`DbMeta` 在打开数据库时从 `db.meta` 文件反序列化,DDL 操作后重新序列化写回。

---

## SmManager — 系统管理器

### 核心字段

```cpp
class SmManager {
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    std::string db_dir_;                    // 当前数据库目录
    DbMeta db_;                             // 当前数据库元数据
    std::unordered_map<std::string, std::unique_ptr<RmFileHandle>> fhs_;  // 表名 -> 记录文件句柄
    std::unordered_map<std::string, std::unique_ptr<IxIndexHandle>> ihs_; // 索引名 -> 索引句柄
};
```

### 核心方法

| 方法 | 行为 |
|---|---|
| `create_db / drop_db` | 创建/删除数据库目录及 `db.meta` |
| `open_db / close_db` | 反序列化/序列化 `db.meta`,打开所有表和索引文件 |
| `create_table` | 创建 `.db` 文件,在 `db_` 中注册 `TabMeta`,计算各列 `offset` 和 `record_size` |
| `drop_table` | 删除表文件与相关索引,从 `db_` 移除 |
| `create_index` | 对指定列创建 B+ 树索引文件 |
| `drop_index` | 删除索引文件 |
| `desc_table` | 打印表的列信息(名字/类型/长度) |
| `show_table` | 打印表的详细信息 |

### 表文件命名约定

| 产物 | 文件名 | 管理器 |
|---|---|---|
| 表记录文件 | `<db_dir>/<table_name>.db` | `RmManager` |
| 索引文件 | `<db_dir>/<table_name>.<col_name>.db` | `IxManager` |
| 数据库元数据 | `<db_dir>/db.meta` | `SmManager` |

---

## DDL 执行流程

```
CREATE TABLE t1 (a INT, b CHAR(10)):
  1. Analyzer 检查表名不重复
  2. SmManager::create_table("t1", cols)
     a. 计算 offset: a=0, b=4, record_size=14
     b. RmManager::create_file("t1.db", 14)
     c. 构造 TabMeta,插入 db_.tabs_
  3. 序列化 db_ 写回 db.meta
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `ColMeta` / `TabMeta` / `DbMeta` | ✅ 已定义 | 框架提供 |
| `create_table` / `desc_table` / `show_table` | ✅ 已实现 | lab1 完成 |
| `open_db` / `close_db` | ⬜ 部分 TODO | lab2 要求完善 |
| `drop_table` / `drop_index` | ⬜ TODO | lab2 要求实现 |
| `create_index` / `drop_index` | ⬜ 部分 | 后续实验 |

> 上一篇:[06-index-manager](06-index-manager.md)
> 下一篇:[08-parser](08-parser.md) — 解析层。
