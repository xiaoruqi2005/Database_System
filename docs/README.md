# RMDB 框架结构文档

本目录自顶向下地记录 RMDB(中国人民大学数据库教学系统)的整体架构、各层模块职责、关键数据结构、核心接口与数据流。文档内容以 `lab1/lab1/src/` 的代码为基准,并标注各层对应的实验。

## 写作约定

| 项目 | 约定 |
|---|---|
| 正文语言 | 中文(讲解、说明、列表) |
| 图示文字 | **英文**(ASCII art、布局图、流程框图),避免等宽字体下中文对齐错位 |
| Mermaid 图 | 可用中文标签(Mermaid 无对齐问题) |
| 代码/字段/方法签名 | 原样 C++ |
| 组织方式 | **自顶向下**:先整体架构 → 逐层深入 |

## 架构总图

```
+=====================================================================+
|                         RMDB Architecture                           |
+=====================================================================+
|                                                                     |
|  +-------------------+   SQL string                                 |
|  |   Client / CLI    | --------------+                              |
|  +-------------------+               |                              |
|                                      v                              |
|  +----------------------------------------------------------------+ |
|  |                       Portal (portal.h)                        | |
|  |  Parser -> Analyzer -> Optimizer -> ExecutionManager           | |
|  +----------------------------------------------------------------+ |
|        |          |           |              |                     |
|        v          v           v              v                     |
|  +----------+ +----------+ +----------+ +----------------+         |
|  | 08-      | | 09-      | | 10-      | | 11-Execution   |         |
|  | Parser   | | Analyzer | | Optimizer| | Engine         |         |
|  | (AST)    | | (Query)  | | (Plan)   | | (Executor tree)|         |
|  +----------+ +----------+ +----------+ +----------------+         |
|                                                  |                 |
|                +---------------------------------+                 |
|                |             |           |      |                 |
|                v             v           v      v                 |
|  +------------------+ +------------+ +---------+ +----------+      |
|  | 07-System        | | 05-Record  | | 06-Index| | 03-      |      |
|  | Manager (Sm)     | | Manager(Rm)| | Manager | | Context  |      |
|  | (Meta/DDL)       | | (Tuple)    | | (B+Tree)| | (Txn/Log)|      |
|  +------------------+ +------------+ +---------+ +----------+      |
|          |                  |             |                         |
|          v                  v             v                         |
|  +----------------------------------------------------------------+ |
|  |                   04-Storage Layer                             | |
|  |        BufferPoolManager <-> DiskManager <-> Page              | |
|  |                    |                                           | |
|  |              Replacer (LRU)                                    | |
|  +----------------------------------------------------------------+ |
|                                  |                                 |
|                                  v                                 |
|                          +----------------+                        |
|                          |  Disk (.db file)|                        |
|                          +----------------+                        |
|                                                                     |
|  Cross-cutting:                                                     |
|  +------------------+  +------------------+  +------------------+   |
|  | 12-Transaction   |  | 13-Recovery      |  | 03-Errors/Defs  |   |
|  | (LockMgr/TxnMgr) |  | (LogMgr/Recovery)|  | (Exceptions)    |   |
|  +------------------+  +------------------+  +------------------+   |
+=====================================================================+
```

## 文档索引(建议自顶向下阅读)

| 编号 | 文档 | 内容 | 对应实验 |
|---|---|---|---|
| — | [README](README.md)(本文) | 文档索引 + 架构总图 | — |
| 01 | [01-architecture-overview](01-architecture-overview.md) | 整体分层架构 + SQL 生命周期 + 分层职责矩阵 | 全局 |
| 02 | [02-project-structure](02-project-structure.md) | 源码目录树 + CMake 构建系统 + 可执行产物 | 全局 |
| 03 | [03-common-infrastructure](03-common-infrastructure.md) | 公共层:config/defs/errors/Context/RecordPrinter | lab1 |
| 04 | [04-storage-layer](04-storage-layer.md) | 存储层:Page/DiskManager/BufferPoolManager/LRU | lab1 |
| 05 | [05-record-manager](05-record-manager.md) | 记录管理层:RmFileHandle/RmManager/RmScan | lab1 |
| 06 | [06-index-manager](06-index-manager.md) | 索引层:B+ 树 IxIndexHandle/IxManager/IxScan | lab1 |
| 07 | [07-system-manager](07-system-manager.md) | 系统管理层:元数据 SmManager/TabMeta/ColMeta | lab1 / lab2 |
| 08 | [08-parser](08-parser.md) | 解析层:flex/bison + AST 节点体系 | 框架提供 |
| 09 | [09-analyzer](09-analyzer.md) | 分析层:语义检查 + 列名解析 + Query 生成 | lab2 |
| 10 | [10-optimizer](10-optimizer.md) | 优化层:Plan 体系 + Planner + Optimizer | lab2 |
| 11 | [11-execution-engine](11-execution-engine.md) | 执行层:火山模型 + 六种 Executor | lab2 |
| 12 | [12-transaction](12-transaction.md) | 事务层:Transaction/LockManager/2PL | lab5 / lab6 |
| 13 | [13-recovery](13-recovery.md) | 恢复层:LogManager/LogRecovery/redo-undo | lab8 |

## 实验 × 架构层对应矩阵

| 架构层 | lab1 | lab2 | lab3 | lab4 | lab5 | lab6 | lab7 | lab8 | lab9 | lab10 |
|---|---|---|---|---|---|---|---|---|---|---|
| 03-Common | ✅ | — | — | — | — | — | — | — | — | — |
| 04-Storage | ✅ | — | — | — | — | — | — | — | — | — |
| 05-Record | ✅ | — | — | — | — | — | — | — | — | — |
| 06-Index | ✅ | — | — | — | — | — | — | — | — | — |
| 07-System | 部分 | ✅ | — | — | — | — | — | — | — | — |
| 08-Parser | 框架 | — | — | — | — | — | — | — | — | — |
| 09-Analyzer | 框架 | ✅ | — | — | — | — | — | — | — | — |
| 10-Optimizer | 框架 | ✅ | — | — | — | — | — | — | — | — |
| 11-Execution | 框架 | ✅ | ✅ | ✅ | — | — | ✅ | — | — | — |
| 12-Transaction | 框架 | — | — | — | ✅ | ✅ | — | — | — | — |
| 13-Recovery | 框架 | — | — | — | — | — | — | ✅ | — | — |

> `✅` 表示该层由对应实验实现;`框架` 表示 RMDB 框架已提供;`部分` 表示部分实现;`—` 表示不涉及或后续扩展。各实验的具体要求以 `labx/labx.md` 为准。

## 数据来源说明

- 文档内容基于 `lab1/lab1/src/` 中已完成的代码(lab1 已实现存储、记录、索引、公共层)。
- 执行层、事务层、恢复层在 `lab1` 代码中为**框架骨架/TODO**,文档如实标注实现状态,不臆造未实现细节。
- 后续实验完成后,应同步更新对应文档的"实现状态"小节。