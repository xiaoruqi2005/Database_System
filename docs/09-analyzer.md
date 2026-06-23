# 09 分析层

> 相关源文件:`src/analyze/analyze.{h,cpp}`

## 一句话职责

分析层对 AST 做**语义分析**:检查表名/列名是否存在、类型是否匹配,解析列名到具体的 `TabMeta`/`ColMeta`,生成供优化器使用的 `Query` 结构。

---

## Query 结构

```cpp
struct Query {
    std::vector<Condition> conds;          // WHERE 条件(已解析列名)
    std::vector<TabCol> sel_cols;          // SELECT 的目标列
    std::vector<std::string> tabs;         // FROM 的表名
    std::unordered_map<std::string, TabMeta> tabs_meta;  // 表名 -> 元数据
    std::vector<SetClause> set_clauses;    // UPDATE 的 SET 子句
    std::vector<Value> values;             // INSERT 的值
};
```

---

## Analyze 类

```cpp
class Analyze {
private:
    SmManager *sm_manager_;
public:
    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root, Context *context);
};
```

`do_analyze` 根据 AST 根节点类型分发到对应的 `visit_*` 方法:

| 方法 | 处理语句 | 关键检查 |
|---|---|---|
| `visit_select` | SELECT | 表存在、列存在、条件中列的类型匹配 |
| `visit_insert` | INSERT | 表存在、值数量与列数匹配、类型匹配 |
| `visit_delete` | DELETE | 表存在、条件合法 |
| `visit_update` | UPDATE | 表存在、SET 的列存在、条件合法 |
| `visit_create_table` | CREATE TABLE | 表不存在、列定义合法 |
| `visit_drop_table` | DROP TABLE | 表存在 |
| `visit_create_index` | CREATE INDEX | 表和列存在 |
| `visit_drop_index` | DROP INDEX | 索引存在 |

---

## 语义检查要点

1. **表名解析**:`FROM` 子句中的表名必须在 `SmManager` 中存在,否则抛 `TableNotFoundError`。
2. **列名解析**:`WHERE`/`SELECT` 中引用的列必须在某个 FROM 表中存在,否则抛 `ColumnNotFoundError`。若列名有表名前缀(`t1.a`),验证该表在 FROM 列表中。
3. **类型检查**:比较条件两端(`a > 10`)的类型必须兼容,否则抛 `TypeError`。
4. **唯一性检查**:若列名在多张表中存在且无前缀,可能需要报歧义错误。

---

## 条件解析(`Condition`)

```cpp
struct Condition {
    TabCol lhs_col;    // {tab_name, col_name} 左操作数
    bool is_rhs_val;   // 右操作数是值还是列
    TabCol rhs_col;    // 右操作数为列时
    Value rhs_val;     // 右操作数为值时
    SvCompOp op;       // 比较运算符
};
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `Analyze` 框架 | ✅ 已定义 | 框架提供 |
| `visit_select` / `visit_insert` / `visit_delete` / `visit_update` | ⬜ 部分 TODO | lab2 要求完善 |
| `visit_create_table` / `visit_drop_table` | ⬜ 部分 TODO | lab2 要求完善 |

> 上一篇:[08-parser](08-parser.md)
> 下一篇:[10-optimizer](10-optimizer.md) — 优化层。
