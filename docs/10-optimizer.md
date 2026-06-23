# 10 优化层

> 相关源文件:`src/optimizer/plan.h`、`src/optimizer/planner.{h,cpp}`、`src/optimizer/optimizer.h`

## 一句话职责

优化层把分析层输出的 `Query` 转换为执行计划 `Plan`,决定使用哪种扫描方式(顺序扫描 vs 索引扫描)、如何组织算子树。在 RMDB 教学框架中,优化器以**基于规则**的简单策略为主。

---

## Plan 节点体系(`plan.h`)

所有 Plan 节点继承自 `AbstractPlanNode`,形成一个算子树:

```
AbstractPlanNode (base)
  |
  +-- DDLPlan            (CREATE/DROP TABLE/INDEX)
  +-- DMLPlan            (INSERT/DELETE/UPDATE)
  +-- SelectPlan         (SELECT)
  +-- ProjectionPlan     (PROJECT, has child plan)
  +-- SeqScanPlan        (全表扫描)
  +-- IndexScanPlan      (索引范围扫描)
  +-- NestedLoopJoinPlan (嵌套循环连接, has left + right child)
  +-- SortPlan           (排序, has child plan)
```

每个 Plan 节点持有:
- 自身的计划信息(目标表、条件、列列表等)
- 子 Plan 指针(`children_`)

---

## Optimizer 入口

```cpp
class Optimizer {
    SmManager *sm_manager_;
    BufferPoolManager *buffer_pool_manager_;
public:
    std::shared_ptr<Plan> plan_query(std::shared_ptr<Query> query, Context *context);
};
```

`plan_query` 根据 `Query` 类型分发:

| Query 类型 | 生成的 Plan |
|---|---|
| SELECT(单表,无索引) | `SelectPlan` → `ProjectionPlan` → `SeqScanPlan` |
| SELECT(单表,条件列有索引) | `SelectPlan` → `ProjectionPlan` → `IndexScanPlan` |
| SELECT(多表) | `SelectPlan` → `ProjectionPlan` → `NestedLoopJoinPlan` → (SeqScanPlan × N) |
| INSERT | `DMLPlan`(INSERT 模式) |
| DELETE | `DMLPlan`(DELETE 模式) |
| UPDATE | `DMLPlan`(UPDATE 模式) |
| CREATE TABLE | `DDLPlan`(CREATE TABLE) |
| DROP TABLE | `DDLPlan`(DROP TABLE) |

---

## Planner — Plan 构造器(`planner.{h,cpp}`)

`Planner` 是实际构造 Plan 树的组件,主要方法:

| 方法 | 行为 |
|---|---|
| `make_plan` | 根据 Query 顶层类型分发 |
| `make_select_plan` | 构造 SELECT 的 Plan 树 |
| `make_dml_plan` | 构造 INSERT/DELETE/UPDATE 的 Plan |
| `make_ddl_plan` | 构造 DDL 的 Plan |
| `make_filter` | 把 `Condition` 列表转为索引扫描或顺序扫描 |

### 单表 SELECT 的 Plan 树

```
SelectPlan
  |
  v
ProjectionPlan { sel_cols: [a, b] }
  |
  v
SeqScanPlan { tab: "t1", conds: [a > 10] }
```

若条件 `a > 10` 中的列 `a` 有索引:

```
SelectPlan
  |
  v
ProjectionPlan { sel_cols: [a, b] }
  |
  v
IndexScanPlan { tab: "t1", index_col: "a", lower: 10, upper: +inf }
```

### 多表 JOIN 的 Plan 树

```
SelectPlan
  |
  v
ProjectionPlan { sel_cols: [t1.a, t2.b] }
  |
  v
NestedLoopJoinPlan { join_cond: t1.id = t2.t1_id }
  / \
 v   v
SeqScanPlan   SeqScanPlan
{tab:"t1"}    {tab:"t2"}
```

---

## 索引选择规则(基于规则)

```
for each condition cond in query.conds:
    if cond.lhs_col has index AND cond.is_rhs_val:
        use IndexScanPlan with [cond.rhs_val, cond.rhs_val]
        remove cond from remaining filter conditions
    else:
        use SeqScanPlan, apply all conds as post-filter

优先级:
  1. 等值条件 (=) 上的索引 → 最优
  2. 范围条件 (>, <, >=, <=) 上的索引 → 次优
  3. 无索引 → 顺序扫描
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `AbstractPlanNode` 及各 Plan 子类 | ✅ 已定义 | 框架提供 |
| `Optimizer::plan_query` | ⬜ 部分 TODO | lab2 要求完善 |
| `Planner::make_select_plan` | ⬜ 部分 TODO | lab2 要求完善 |
| `Planner::make_dml_plan` | ⬜ 部分 TODO | lab2 要求完善 |
| `Planner::make_ddl_plan` | ⬜ 部分 TODO | lab2 要求完善 |
| 索引选择优化 | ⬜ TODO | 后续实验可能扩展 |

> 上一篇:[09-analyzer](09-analyzer.md)
> 下一篇:[11-execution-engine](11-execution-engine.md) — 执行层。
