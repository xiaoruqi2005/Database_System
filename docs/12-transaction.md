# 12 事务层

> 相关源文件:`src/transaction/transaction.h`、`src/transaction/transaction_manager.{h,cpp}`、`src/transaction/txn_defs.h`、`src/transaction/concurrency/lock_manager.{h,cpp}`

## 一句话职责

事务层提供 **ACID** 中的隔离性(Isolation)和原子性(Atomicity)支撑,通过两阶段封锁(2PL)实现并发控制,通过事务对象跟踪每个事务持有的锁和写操作集合。

---

## Transaction — 事务对象

```cpp
class Transaction {
    txn_id_t txn_id_;                                    // unique transaction id
    std::atomic<txn_id_t> thread_id_;                    // thread running this txn
    IsolationLevel isolation_level_;                     // isolation level
    std::atomic<CNT> lock_set_;                          // count of locks held (for deadlock detection)
    std::shared_ptr<std::unordered_set<PageSlot>> s_lock_set_;  // shared locks held
    std::shared_ptr<std::unordered_set<PageSlot>> x_lock_set_;  // exclusive locks held
    std::shared_ptr<std::vector<WriteRecord>> write_set_;       // undo log (for rollback)
    std::atomic<lsn_t> prev_lsn_;                        // previous log record LSN
public:
    txn_id_t get_txn_id();
    void append_write_record(WriteRecord wr);
    void pop_write_record();
    void append_s_lock(Rid rid);
    void append_x_lock(Rid rid);
};
```

### 字段含义

| 字段 | 用途 |
|---|---|
| `txn_id_` | 全局唯一递增事务 ID |
| `isolation_level_` | 隔离级别(READ_COMMITTED / REPEATABLE_READ / SERIALIZABLE) |
| `s_lock_set_` / `x_lock_set_` | 当前事务持有的共享锁/排他锁集合(用于解锁) |
| `write_set_` | 记录所有写操作(INSERT/DELETE/UPDATE),用于 abort 时 undo |
| `prev_lsn_` | 该事务上一条日志的 LSN,用于日志链表遍历 |

---

## LockManager — 锁管理器

### 核心数据结构

```cpp
class LockManager {
    std::atomic<txn_id_t> next_txn_id_;
    std::atomic<bool> deadlock_detection_enabled_;
    std::unordered_map<Rid, std::list<LockRequest>> lock_table_;  // Rid -> lock queue
    std::mutex latch_;
};
```

### LockRequest

```cpp
struct LockRequest {
    txn_id_t txn_id_;
    LockMode lock_mode_;    // S_LOCK or X_LOCK
    bool granted_;          // whether the lock is granted
};
```

### 核心方法

| 方法 | 行为 |
|---|---|
| `lock_shared(table, rid, txn)` | 对记录加共享锁;若有排他锁则等待 |
| `lock_exclusive(table, rid, txn)` | 对记录加排他锁;若任何锁存在则等待 |
| `unlock(table, rid, txn)` | 释放事务对该记录的锁;唤醒等待者 |
| `is_lock_expired(txn)` | 检测死锁超时(Wait-Die / Wound-Wait) |

### 死锁预防策略

RMDB 教学框架通常采用 **Wound-Wait** 或 **Wait-Die** 策略:

```
Wound-Wait (old kills young):
  - T_old 请求锁,若被 T_young 持有 → T_young abort
  - T_young 请求锁,若被 T_old 持有 → T_young 等待

Wait-Die (young waits for old):
  - T_old 请求锁,若被 T_young 持有 → T_old 等待
  - T_young 请求锁,若被 T_old 持有 → T_young abort
```

---

## TransactionManager — 事务管理器

### 核心方法

| 方法 | 行为 |
|---|---|
| `begin(isolation_level)` | 创建新 Transaction,分配 txn_id |
| `commit(txn)` | 释放所有锁,清理事务状态,刷日志 |
| `abort(txn)` | 遍历 `write_set_` 做 undo;释放所有锁 |

### Commit 流程

```
commit(txn):
  1. flush log buffer to disk (up to prev_lsn_ of txn)
  2. release all S/X locks in txn's lock sets
  3. clean up txn state
```

### Abort 流程

```
abort(txn):
  1. iterate write_set_ in reverse order (LIFO):
     - INSERT record: delete it
     - DELETE record: re-insert it
     - UPDATE record: restore old value
  2. release all S/X locks
  3. mark txn as aborted
```

---

## 两阶段封锁(2PL)协议

```
   Growing Phase         |    Shrinking Phase
                         |
   acquire S/X locks     |    release locks
   (no release allowed)  |    (no acquire allowed)
                         |
   ----------------------|------------------------
                         ^ COMMIT triggers shrink
```

在严格 2PL(S2PL)中,所有排他锁在事务 COMMIT 后才释放,保证可恢复性。

---

## WriteRecord — undo 日志项

```cpp
struct WriteRecord {
    WType wtype;          // INSERT_RECORD / DELETE_RECORD / UPDATE_RECORD
    std::string table_name_;
    Rid rid_;
    char *old_value_;     // for UPDATE undo
    int len_;
};
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `Transaction` 对象 | ✅ 已定义 | 框架提供 |
| `LockManager`(lock_shared/exclusive/unlock) | ⬜ TODO | lab5 要求实现 |
| `LockManager` 死锁检测 | ⬜ TODO | lab6 要求实现 |
| `TransactionManager`(begin/commit/abort) | ⬜ TODO | lab5 要求实现 |
| 2PL 协议集成到执行层 | ⬜ TODO | lab5/lab6 |

> 上一篇:[11-execution-engine](11-execution-engine.md)
> 下一篇:[13-recovery](13-recovery.md) — 恢复层。
