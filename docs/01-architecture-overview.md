# 01 整体架构概览

> 相关源文件:`src/rmdb.cpp`、`src/portal.h`、`src/defs.h`、`src/errors.h`、`src/common/config.h`

## 一句话职责

RMDB 是一个分层的关系型数据库教学系统。最上层 `Portal` 把 SQL 字符串依次交给解析器、分析器、优化器、执行器;执行器通过系统管理器、记录管理器、索引管理器操作数据,最终落到缓冲池与磁盘。

## 全局管理器拓扑

`rmdb.cpp` 的 `main` 函数自底向上构造一组全局单例,通过构造函数依赖注入:

```
DiskManager (disk I/O)
     |
     v
BufferPoolManager (page cache, LRU)
     |          |
     v          v
RmManager   IxManager
 (record)   (index)
     |          |
     +----+-----+
          |
          v
      SmManager (schema/meta)
          |
LockManager ---+
               |
TransactionManager
               |
          Context (per-query)
```

各管理器的职责:

| 管理器 | 头文件 | 职责 | 依赖 |
|---|---|---|---|
| `DiskManager` | `storage/disk_manager.h` | 文件读写、fd 管理 | 无 |
| `BufferPoolManager` | `storage/buffer_pool_manager.h` | 页面缓存、固定/淘汰 | `DiskManager`、`Replacer` |
| `RmManager` | `record/rm_manager.h` | 记录文件的创建/打开/销毁 | `DiskManager`、`BufferPoolManager` |
| `IxManager` | `index/ix_manager.h` | B+ 树索引文件的创建/打开/销毁 | `DiskManager`、`BufferPoolManager` |
| `SmManager` | `system/sm_manager.h` | 数据库/表/索引元数据与 DDL | `RmManager`、`IxManager` |
| `LockManager` | `transaction/concurrency/lock_manager.h` | 两阶段封锁、死锁预防 | 无 |
| `TransactionManager` | `transaction/transaction_manager.h` | 事务生命周期、txn_id 分配 | `LockManager` |
| `LogManager` | `recovery/log_manager.h` | WAL 日志记录与刷盘 | `DiskManager` |
| `LogRecovery` | `recovery/log_recovery.h` | 重启后 redo/undo 恢复 | `DiskManager` |

## Portal 层:SQL 的中枢调度

`Portal`(`src/portal.h`)是连接各处理阶段的门面。一条 SQL 字符串的处理流水线:

```
SQL string
   |
   v
1. Parser::do_str_parse(sql)        --> ast::TreeNode (AST)
   |
   v
2. Analyze::do_analyze(ast, ctx)    --> Query (semantic representation)
   |
   v
3. Optimizer::plan_query(query, ctx)--> std::shared_ptr<Plan>
   |
   v
4. QlManager::run_mutli_query / select_from / run_dml
   --> builds Executor tree (volcano model)
   --> executor.Init() + executor.Next() loop
   --> RecordPrinter outputs results
```

Portal 对外暴露的关键方法:

| 方法 | 阶段 |
|---|---|
| `do_str_parse(sql)` | 解析:SQL → AST |
| `do_analyze(ast, ctx)` | 分析:AST → Query |
| `plan_query(query, ctx)` | 优化:Query → Plan |
| `run_mutli_query / select_from / run_dml` | 执行:Plan → Executor → 结果 |

## 一条 SELECT 的完整生命周期

以 `SELECT a, b FROM t1 WHERE a > 10` 为例:

```
+--------+    SQL text     +--------+    AST      +----------+
| Client | --------------> | Parser | ----------> | Analyzer |
+--------+                  +--------+             +----------+
                                                        |
                                          Query{cols, tabs, conds}
                                                        v
                                                 +-----------+
                                                 | Optimizer |
                                                 +-----------+
                                                        |
                                              SelectPlan{cols, conds, tabs}
                                                        v
                                            +-------------------+
                                            | ExecutionManager  |
                                            +-------------------+
                                                        |
                                    builds executor tree (volcano model):
                                                        v
                                   +-------------------+
                                   |  ProjectionExec   |  (outputs a, b)
                                   +-------------------+
                                            |
                                   +-------------------+
                                   |   SeqScanExec     |  (scans t1)
                                   +-------------------+
                                            |
                                   calls RmFileHandle to read tuples,
                                   applies condition a > 10
                                            |
                                            v
                                   RecordPrinter prints result rows
```

## DDL/DML/事务控制语句分流

Portal 根据 AST 的根节点类型,把语句分流到不同的分析/执行路径:

| 语句类型 | AST 根节点 | 分析入口 | 执行入口 |
|---|---|---|---|
| SELECT | `SvSelectStmt` | `visit_select` | `select_from` |
| INSERT | `SvInsertStmt` | `visit_insert` | `run_dml` |
| DELETE | `SvDeleteStmt` | `visit_delete` | `run_dml` |
| UPDATE | `SvUpdateStmt` | `visit_update` | `run_dml` |
| CREATE TABLE | `SvCreateTableStmt` | `visit_create_table` | `run_mutli_query`(DDL) |
| DROP TABLE | `SvDropTableStmt` | `visit_drop_table` | `run_mutli_query`(DDL) |
| BEGIN | `TxnBegin` | — | `TransactionManager::begin` |
| COMMIT | `TxnCommit` | — | `TransactionManager::commit` |
| ABORT/ROLLBACK | `TxnAbort` | — | `TransactionManager::abort` |

## 分层职责矩阵

| 层 | 模块 | 输入 | 输出 | 对应文档 |
|---|---|---|---|---|
| 客户端 | CLI / rmdb_client | 用户输入 | SQL 字符串 | 02 |
| 解析 | Parser | SQL 字符串 | AST | 08 |
| 分析 | Analyzer | AST | Query | 09 |
| 优化 | Optimizer | Query | Plan | 10 |
| 执行 | ExecutionEngine | Plan | 记录流/副作用 | 11 |
| 系统管理 | SmManager | DDL | 元数据/文件 | 07 |
| 记录 | RmManager | Rid/记录 | 页面数据 | 05 |
| 索引 | IxManager | 键值 | Rid | 06 |
| 存储 | BufferPoolManager | PageId | Page | 04 |
| 磁盘 | DiskManager | fd/page_no | 字节流 | 04 |
| 事务 | TransactionManager | BEGIN/COMMIT | Transaction | 12 |
| 恢复 | LogManager | 写操作 | 日志记录 | 13 |

## 关键全局类型(`defs.h`)

| 类型 | 定义 | 用途 |
|---|---|---|
| `Rid` | `struct { int page_no; int slot_no; }` | 记录定位符(页号 + 槽号) |
| `ColType` | `enum { TYPE_INT, TYPE_FLOAT, TYPE_STRING }` | 列类型 |
| `RecScan` | 抽象基类 | 扫描器接口:`next()`、`is_end()`、`rid()` |

`config.h` 还定义了类型别名 `frame_id_t`、`page_id_t`、`lsn_t`,以及常量 `PAGE_SIZE=4096`、`BUFFER_POOL_SIZE`、`INVALID_PAGE_ID=-1` 等(详见 [03-common-infrastructure](03-common-infrastructure.md))。

## 全局异常体系(`errors.h`)

RMDB 使用自定义异常类来表达错误,所有异常继承 `std::exception`。执行层和分析层通过抛出/捕获这些异常来处理非法输入和内部错误。

| 异常类 | 含义 | 典型场景 |
|---|---|---|
| `InternalError` | 内部一致性错误 | 不应到达的代码路径 |
| `RMDBError` | RMDB 通用错误基类 | — |
| `IndexNotFoundError` | 索引不存在 | 使用的索引名未建立 |
| `ColumnNotFoundError` | 列不存在 | SQL 中引用了不存在的列 |
| `TableNotFoundError` | 表不存在 | SQL 中引用了不存在的表 |
| `TableExistsError` | 表已存在 | CREATE TABLE 重名 |
| `IndexExistsError` | 索引已存在 | CREATE INDEX 重名 |
| `TypeError` | 类型不匹配 | 比较操作两端类型不一致 |
| `DatabaseNotFoundError` | 数据库不存在 | OPEN 不存在的 db |
| `DatabaseExistsError` | 数据库已存在 | CREATE DATABASE 重名 |
| `InvalidAggregationError` | 非法聚合 | lab3/lab4 扩展 |
| `TransactionAbortException` | 事务中止 | 死锁/缩减期加锁 |

> 异常类均携带 `GetMsg()` / `what()` 返回可读的错误描述,Portal 层捕获后输出 `failure`。

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| DiskManager / BufferPoolManager | ✅ 已实现 | lab1 完成 |
| RmManager / RmFileHandle | ✅ 已实现 | lab1 完成 |
| IxManager / IxIndexHandle | ✅ 已实现 | lab1 完成 |
| SmManager(create/desc/show) | ✅ 已实现 | lab1 完成 |
| SmManager(drop/open/close db) | ⬜ TODO | lab2 要求实现 |
| Parser | ✅ 框架提供 | flex/bison 生成 |
| Analyzer | ⬜ 部分 TODO | lab2 要求完善 |
| Optimizer / Planner | ⬜ 部分 TODO | lab2 要求完善 |
| ExecutionEngine | ⬜ 部分 TODO | lab2 要求实现各 Executor |
| LockManager / TransactionManager | ⬜ TODO | lab5/lab6 |
| LogManager / LogRecovery | ⬜ TODO | lab8 |

> 下一篇:[02-project-structure](02-project-structure.md) — 源码目录树与构建系统。