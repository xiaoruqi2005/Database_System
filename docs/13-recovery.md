# 13 恢复层

> 相关源文件:`src/recovery/log_defs.h`、`src/recovery/log_manager.{h,cpp}`、`src/recovery/log_recovery.{h,cpp}`

## 一句话职责

恢复层实现 **Write-Ahead Logging(WAL)**,在事务修改数据页前先把日志记录写入日志缓冲;数据库崩溃重启后,通过 redo(重做)和 undo(回滚)将数据库恢复到一致状态。

---

## WAL 原则

```
Modified Page   |   Log Record
  flushed to        must be flushed to
  disk              disk FIRST
       ^                 ^
       |                 |
       +---- BEFORE ------+
```

**规则:数据页写回磁盘之前,对应的日志记录必须已经刷盘。**

---

## LogRecord — 日志记录(`log_defs.h`)

```cpp
struct LogRecord {
    lsn_t lsn_;              // log sequence number (unique, monotonic)
    txn_id_t txn_id_;        // which transaction
    lsn_t prev_lsn_;         // previous LSN of same txn (chain)
    LogRecordType log_type_; // type of log record
    // type-specific fields:
    std::string table_name_;
    Rid rid_;
    int log_tot_len_;        // total serialized length
    int log_ser_len_;        // header length
};
```

### LogRecordType

| 类型 | 含义 |
|---|---|
| `BEGIN` | 事务开始 |
| `COMMIT` | 事务提交 |
| `ABORT` | 事务中止 |
| `INSERT` | 插入记录(记录新值,redo 用) |
| `DELETE` | 删除记录(记录旧值,undo 用) |
| `UPDATE` | 更新记录(记录旧值和新值,redo + undo) |
| `CHECKPOINT` | 检查点(记录活跃事务表) |

---

## LogManager — 日志管理器

### 核心字段

```cpp
class LogManager {
    DiskManager *disk_manager_;
    std::atomic<lsn_t> next_lsn_;          // global LSN counter
    std::atomic<bool> enable_logging_;     // logging on/off
    std::atomic<int> log_size_;            // current buffer offset
    std::condition_variable cv_;           // for flush notification
    char *log_buffer_;                     // in-memory log buffer (LOG_BUFFER_SIZE)
    char *flush_buffer_;                   // buffer being flushed
    lsn_t persistent_lsn_;                 // last flushed LSN
    std::unordered_map<txn_id_t, lsn_t> active_txn_;  // active txns and their last LSN
};
```

### 核心方法

| 方法 | 行为 |
|---|---|
| `AppendLogRecord(LogRecord)` | 序列化日志记录到 `log_buffer_`,返回分配的 LSN |
| `FlushThread()` | 后台线程:`enable_logging_` 为 true 时,周期性把 `log_buffer_` 刷盘到 `db.log` |
| `flush_log_to_disk()` | 强制把 `log_buffer_` 刷盘(如 COMMIT 时) |

### AppendLogRecord 流程

```
AppendLogRecord(record):
  1. lsn = next_lsn_++
  2. record.lsn_ = lsn; record.prev_lsn_ = txn.prev_lsn_
  3. txn.prev_lsn_ = lsn
  4. serialize record into log_buffer_ at offset log_size_
  5. log_size_ += record.log_tot_len_
  6. active_txn_[txn_id] = lsn
  7. return lsn
```

---

## LogRecovery — 恢复管理器(`log_recovery.h`)

### 核心字段

```cpp
class LogRecovery {
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    std::unordered_map<txn_id_t, lsn_t> active_txn_;   // active txns during redo
    std::unordered_map<txn_id_t, lsn_t> active_q_;      // txns to undo
    lsn_t lsn_;                                         // current LSN being processed
};
```

### 重启恢复流程(ARIES 协议)

```
Restart recovery (ARIES):

  Phase 1: ANALYSIS
    - scan log from beginning (or last checkpoint)
    - build active_txn_ table
    - determine redo_start_lsn

  Phase 2: REDO
    - scan log forward from redo_start_lsn
    - for each INSERT/UPDATE log:
        re-apply the change to the data page
    - redo all logged operations (even aborted txns)

  Phase 3: UNDO
    - scan log backward
    - for each INSERT/DELETE/UPDATE of uncommitted txn:
        reverse the change
    - write CLR (Compensation Log Record)
```

### Redo 逻辑

```cpp
void redo() {
    for each LogRecord in log (forward):
        switch(record.log_type_) {
            case INSERT:
                fh->insert_record(record.new_value)  // re-insert
                break;
            case DELETE:
                fh->delete_record(record.rid)        // re-delete
                break;
            case UPDATE:
                fh->update_record(record.rid, record.new_value)  // re-update
                break;
            case COMMIT:
                active_txn_.erase(record.txn_id_)    // committed, no undo needed
                break;
        }
}
```

### Undo 逻辑

```cpp
void undo() {
    for each LogRecord in log (backward) for txn in active_q_:
        switch(record.log_type_) {
            case INSERT:
                fh->delete_record(record.rid)        // undo insert = delete
                break;
            case DELETE:
                fh->insert_record(record.old_value)  // undo delete = re-insert
                break;
            case UPDATE:
                fh->update_record(record.rid, record.old_value)  // restore old
                break;
        }
}
```

---

## 日志文件格式

```
db.log:
+----------+----------+----------+----------+
| LogRec 1 | LogRec 2 | LogRec 3 | ...      |
| (BEGIN)  | (INSERT) | (COMMIT) |          |
+----------+----------+----------+----------+

Each record is variable-length, self-describing:
  [lsn][txn_id][prev_lsn][type][data...][tot_len]
```

---

## 检查点(Checkpoint)

RMDB 教学框架可使用**非精致检查点**:
- 周期性记录当前活跃事务表
- 减少恢复时需要扫描的日志量

```
CHECKPOINT log record:
  { active_txns: [T1, T3, ...], their_last_lsns: [...] }
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `LogRecord` 及序列化 | ✅ 已定义 | 框架提供 |
| `LogManager::AppendLogRecord` | ⬜ TODO | lab8 要求实现 |
| `LogManager::flush_log_to_disk` | ⬜ TODO | lab8 要求实现 |
| `LogRecovery::redo` | ⬜ TODO | lab8 要求实现 |
| `LogRecovery::undo` | ⬜ TODO | lab8 要求实现 |
| WAL 集成到执行层 | ⬜ TODO | lab8 |

> 上一篇:[12-transaction](12-transaction.md)
> 返回:[README](README.md) — 文档总索引。
