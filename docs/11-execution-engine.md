# 11 执行层

> 相关源文件:`src/execution/execution_manager.{h,cpp}`、`src/execution/executor_abstract.h`、`src/execution/executor_seq_scan.h`、`src/execution/executor_index_scan.h`、`src/execution/executor_insert.h`、`src/execution/executor_delete.h`、`src/execution/executor_update.h`、`src/execution/executor_projection.h`、`src/execution/executor_nestedloop_join.h`、`src/execution/execution_sort.h`、`src/execution/execution_defs.h`

## 一句话职责

执行层采用**火山模型**(Volcano Model),把优化层生成的 `Plan` 树转换为一棵对应的 `Executor` 树,通过 `Init()` + `Next()` 迭代协议逐行产出结果。

---

## 火山模型(迭代器模型)

每个 Executor 实现统一的迭代接口:

```cpp
class AbstractExecutor {
public:
    virtual void Init() = 0;
    virtual bool Next(Tuple *tuple, Rid *rid) = 0;  // return false when exhausted
    virtual const Schema *GetSchema() = 0;
};
```

数据按需拉取(pull-based),父算子调用子算子的 `Next()` 获取一行:

```
Next() call flow (pull-based):

  ProjectionExec.Next(tuple)
      --> calls child.Next(raw_tuple)
          --> SeqScanExec.Next(raw_tuple)
              --> RmScan.next() to advance
              --> RmFileHandle.get_record(rid) to read tuple
              <-- returns true with data
          <-- applies WHERE condition filter
          <-- returns true if passed, false to try next
      <-- projects columns, fills output tuple
      <-- returns true
```

---

## ExecutionManager — 执行管理器

```cpp
class QlManager {
    SmManager *sm_manager_;
    RmManager *rm_manager_;
    IxManager *ix_manager_;
    TransactionManager *txn_mgr_;
public:
    std::unique_ptr<AbstractExecutor> create_executor(Plan *plan, Context *context);
    void run_mutli_query(Query *query, Context *context);   // DDL
    void select_from(Query *query, Context *context);       // SELECT (prints results)
    void run_dml(Query *query, Context *context);           // INSERT/DELETE/UPDATE
};
```

`create_executor` 根据 Plan 类型递归构造 Executor 树。

---

## 六种核心 Executor

### 1. SeqScanExecutor(顺序扫描)

```cpp
class SeqScanExecutor : public AbstractExecutor {
    RmScan *rm_scan_;        // 底层记录扫描器
    RmFileHandle *fh_;       // 表文件句柄
    std::vector<Condition> conds_;  // 过滤条件
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 初始化 `RmScan`,定位到第一条记录 |
| `Next()` | 从 `RmScan` 取下一条记录;若满足所有 `conds_` 则返回 true;否则继续扫描直到文件末尾 |

### 2. IndexScanExecutor(索引扫描)

```cpp
class IndexScanExecutor : public AbstractExecutor {
    IxScan *ix_scan_;       // 底层索引范围扫描器
    IxIndexHandle *ih_;     // 索引句柄
    RmFileHandle *fh_;      // 表文件句柄(用 rid 回表取记录)
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 用 `lower_bound`/`upper_bound` 初始化 `IxScan` |
| `Next()` | 从 `IxScan` 取下一个 `rid`;用 `fh_->get_record(rid)` 回表取完整记录 |

### 3. InsertExecutor(插入)

```cpp
class InsertExecutor : public AbstractExecutor {
    RmFileHandle *fh_;
    std::vector<Value> values_;
    TabMeta *tab_;
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 准备插入数据 |
| `Next()` | 调用 `fh_->insert_record(buf)` 插入一条记录;若有索引,同步调用 `ih_->insert_entry(key, rid)`;返回 false(插入是一次性操作) |

### 4. DeleteExecutor(删除)

```cpp
class DeleteExecutor : public AbstractExecutor {
    std::unique_ptr<AbstractExecutor> child_;  // 产生待删记录的子算子
    RmFileHandle *fh_;
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 初始化子算子 |
| `Next()` | 循环调用 `child_->Next()` 收集所有待删 rid;逐条调用 `fh_->delete_record(rid)` 和 `ih_->delete_entry(key)` |

### 5. UpdateExecutor(更新)

```cpp
class UpdateExecutor : public AbstractExecutor {
    std::unique_ptr<AbstractExecutor> child_;
    RmFileHandle *fh_;
    std::vector<SetClause> set_clauses_;
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 初始化子算子 |
| `Next()` | 循环取子算子记录;对每个 `SetClause` 修改记录对应字段;调用 `fh_->update_record(rid, buf)` |

### 6. ProjectionExecutor(投影)

```cpp
class ProjectionExecutor : public AbstractExecutor {
    std::unique_ptr<AbstractExecutor> child_;
    std::vector<TabCol> sel_cols_;   // SELECT 的目标列
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 初始化子算子 |
| `Next()` | 调用 `child_->Next(raw_tuple)`;根据 `sel_cols_` 从 raw_tuple 中提取对应列,组装输出 tuple |

### 7. NestedLoopJoinExecutor(嵌套循环连接)

```cpp
class NestedLoopJoinExecutor : public AbstractExecutor {
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    Condition join_cond_;
    bool left_done_;   // 是否已遍历完左表
};
```

| 步骤 | 行为 |
|---|---|
| `Init()` | 初始化左右子算子 |
| `Next()` | 对左表每条记录,遍历右表所有记录;检查连接条件;匹配则拼接输出 |

---

## 执行流程(SELECT 为例)

```
select_from(query, ctx):
  1. plan = optimizer.plan_query(query, ctx)
  2. exec_tree = create_executor(plan, ctx)
  3. exec_tree->Init()
  4. while exec_tree->Next(&tuple, &rid):
       printer.print_record(tuple)
  5. printer finish
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `AbstractExecutor` 基类 | ✅ 已定义 | 框架提供 |
| `SeqScanExecutor` | ⬜ TODO | lab2 要求实现 |
| `IndexScanExecutor` | ⬜ TODO | lab2 要求实现 |
| `InsertExecutor` | ⬜ TODO | lab2 要求实现 |
| `DeleteExecutor` | ⬜ TODO | lab2 要求实现 |
| `UpdateExecutor` | ⬜ TODO | lab2 要求实现 |
| `ProjectionExecutor` | ⬜ TODO | lab2 要求实现 |
| `NestedLoopJoinExecutor` | ⬜ TODO | lab2 要求实现 |
| `SortExecutor` | ⬜ TODO | 后续实验扩展 |
| `create_executor` | ⬜ TODO | lab2 要求实现 |

> 上一篇:[10-optimizer](10-optimizer.md)
> 下一篇:[12-transaction](12-transaction.md) — 事务层。
