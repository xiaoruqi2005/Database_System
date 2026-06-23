# 03 公共基础设施层

> 相关源文件:`src/common/config.h`、`src/common/context.h`、`src/common/common.h`、`src/defs.h`、`src/errors.h`、`src/record_printer.h`

## 一句话职责

公共层为所有上层模块提供全局类型别名、编译期常量、跨模块传递的运行时上下文 `Context`、统一的异常体系,以及查询结果格式化打印工具。

---

## config.h — 全局类型别名与常量

### 类型别名

| 别名 | 底层类型 | 用途 |
|---|---|---|
| `frame_id_t` | `int32_t` | 缓冲池帧号 |
| `page_id_t` | `int32_t` | 页号 |
| `lsn_t` | `int32_t` | 日志序列号 |
| `slot_offset_t` | `size_t` | 槽偏移 |

### 编译期常量

| 常量 | 值 | 用途 |
|---|---|---|
| `INVALID_FRAME_ID` | `-1` | 无效帧号 |
| `INVALID_PAGE_ID` | `-1` | 无效页号 |
| `HEADER_PAGE_ID` | `0` | 文件头页号 |
| `PAGE_SIZE` | `4096`(4KB) | 每页字节数 |
| `BUFFER_POOL_SIZE` | `65536` | 缓冲池可缓存页数 |
| `LOG_BUFFER_SIZE` | `1024 * PAGE_SIZE` | 日志缓冲区大小 |
| `REPLACER_TYPE` | `"LRU"` | 默认替换策略 |
| `DB_META_NAME` | `"db.meta"` | 数据库元数据文件名 |
| `LOG_FILE_NAME` | `"db.log"` | 日志文件名 |

---

## defs.h — 核心数据类型

### `Rid` — 记录定位符

```cpp
struct Rid {
    int page_no;   // record page number
    int slot_no;   // slot number within page
    bool operator==(const Rid &other) const;
    bool operator!=(const Rid &other) const;
};
```

`Rid` 是贯穿记录层、索引层、执行层的"记录地址",定位一条记录在某个记录文件中的物理位置。

### `Iid` — 索引项定位符

```cpp
struct Iid {
    int page_no;   // index page number
    int slot_no;   // slot within index page
};
```

结构与 `Rid` 相同,但语义不同:`Iid` 定位的是 B+ 树中的一个键值项。

### `ColType` — 列类型枚举

```cpp
enum ColType {
    TYPE_INT,      // 4 bytes
    TYPE_FLOAT,    // 4 bytes
    TYPE_STRING,   // variable length
};
```

`defs.h` 还为 `ColType` 重载了 `<<` / `>>`,使其可以直接以 `int` 形式序列化到元数据文件。

### `RecScan` — 扫描器抽象基类

```cpp
class RecScan {
public:
    virtual ~RecScan() = default;
    virtual void next() = 0;          // advance to next record
    virtual bool is_end() const = 0;  // reached end?
    virtual Rid rid() const = 0;      // current record's Rid
};
```

`RmScan` 和 `IxScan` 都继承 `RecScan`,使执行层的扫描算子可以统一地遍历记录或索引。

---

## errors.h — 异常体系

所有异常继承自 `std::exception`,形成一棵树:

```
std::exception
  |
  +-- InternalError            (internal consistency error)
  +-- RMDBError                (RMDB generic error base)
        |
        +-- IndexNotFoundError
        +-- ColumnNotFoundError
        +-- TableNotFoundError
        +-- TableExistsError
        +-- IndexExistsError
        +-- TypeError
        +-- DatabaseNotFoundError
        +-- DatabaseExistsError
        +-- InvalidAggregationError
  +-- TransactionAbortException
```

每个异常类提供:
- 构造函数接收错误信息字符串
- `GetMsg()` 返回可读消息
- `what()` 返回 `const char*`(供顶层 `catch` 打印)

> Portal 层在 `main` 循环里 `catch (const std::exception &e)`,捕获到任何异常后向 `output.txt` 输出 `failure`。

### 异常含义速查

| 异常 | 触发场景 |
|---|---|
| `ColumnNotFoundError` | SQL 引用了表中不存在的列 |
| `TableNotFoundError` | SQL 引用了未打开/不存在的表 |
| `TableExistsError` | CREATE TABLE 与已有表重名 |
| `IndexExistsError` | 已有相同列的索引 |
| `IndexNotFoundError` | 指定的索引不存在 |
| `TypeError` | 条件比较两端类型不一致 |
| `DatabaseNotFoundError` | 打开不存在的数据库 |
| `DatabaseExistsError` | 创建已存在的数据库 |
| `TransactionAbortException` | 事务中止(死锁/缩减期加锁),携带 `txn_id` 与 `abort_reason` |

---

## context.h — 运行时上下文 `Context`

```cpp
class Context {
public:
    Context(LockManager *lock_mgr, LogManager *log_mgr, Transaction *txn,
            char *_log_buffer = nullptr, int _log_offset = 0)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),
          log_buffer_(_log_buffer), log_offset_(_log_offset) {}

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    Transaction *txn_;
    char *log_buffer_;
    int log_offset_;
};
```

### 字段含义

| 字段 | 用途 |
|---|---|
| `lock_mgr_` | 当前查询用的锁管理器(并发控制) |
| `log_mgr_` | 当前查询用的日志管理器(WAL) |
| `txn_` | 当前事务对象(持有写记录、锁集合) |
| `log_buffer_` | 日志缓冲区(执行层写入日志记录时复用) |
| `log_offset_` | 日志缓冲区当前偏移 |

### 使用方式

`Context` 由 Portal/执行层在执行一条语句时构造,沿着 `Plan → Executor → RmFileHandle/IxIndexHandle` 一路传递。底层记录/索引操作通过它来实现:
- **加锁**:操作记录前调用 `lock_mgr_->lock`。
- **记日志**:修改数据前先写日志记录到 `log_mgr_`。

在 lab1(无并发无恢复)阶段,`Context` 各字段可为空指针。

---

## record_printer.h — 查询结果打印 `RecordPrinter`

```cpp
class RecordPrinter {
public:
    RecordPrinter(int num_col);
    void print_separator();              // print "+----+----+"
    void print_record(const std::vector<std::string> &row);
    void print_header(const std::vector<std::string> &headers);
    void print_record_now(const std::vector<std::string> &row);
};
```

### 用途

`RecordPrinter` 把 SELECT 的结果按表格形式输出到 `output.txt`:

```
+----+----+
| a  | b  |
+----+----+
| 1  | xx |
+----+----+
```

执行层的 `ProjectionExecutor` 完成迭代后,用 `RecordPrinter` 打印所有结果行。列数在构造时确定,后续 `print_record` 必须传入等长的字符串向量。

---

## common.h — 辅助宏

`common.h` 提供少量通用宏与工具函数(如断言宏、序列化辅助),不同版本略有差异。核心是保证各模块共享一致的位操作和断言行为。

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `config.h` 类型别名与常量 | ✅ 已实现 | 框架提供 |
| `defs.h`(Rid/ColType/RecScan) | ✅ 已实现 | 框架提供 |
| `errors.h` 异常体系 | ✅ 已实现 | 框架提供 |
| `Context` | ✅ 已定义 | 字段在 lab1 阶段可为空 |
| `RecordPrinter` | ✅ 已实现 | 框架提供 |

> 上一篇:[02-project-structure](02-project-structure.md)
> 下一篇:[04-storage-layer](04-storage-layer.md) — 存储层:Page、DiskManager、缓冲池。