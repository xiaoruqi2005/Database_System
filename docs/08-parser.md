# 08 解析层

> 相关源文件:`src/parser/parser.h`、`src/parser/ast.{h,cpp}`、`src/parser/parse_node.h`、`src/parser/lex.l`、`src/parser/yacc.y`

## 一句话职责

解析层将 SQL 字符串转换为抽象语法树(AST),供分析层做语义检查。解析层由 **flex**(词法)和 **bison**(语法)生成。

---

## AST 节点体系(`ast.h`)

所有 AST 节点继承自 `ast::TreeNode`:

```
TreeNode (base)
  |
  +-- SvExpr (表达式基类)
  |     +-- SvBinaryOp    (left OP right, e.g. a > 10)
  |     +-- SvColumn      (column reference)
  |     +-- SvLiteral     (INT/FLOAT/STRING 常量)
  |
  +-- SvStmt (语句基类)
        +-- SvSelectStmt    { cols, tabs, conds, group_by?, having?, order_by? }
        +-- SvInsertStmt    { tab_name, values }
        +-- SvDeleteStmt    { tab_name, conds }
        +-- SvUpdateStmt    { tab_name, set_clauses, conds }
        +-- SvCreateTableStmt { tab_name, cols }
        +-- SvDropTableStmt   { tab_name }
        +-- SvCreateIndexStmt { tab_name, col_name }
        +-- SvDropIndexStmt   { tab_name, col_name }
```

### 控制语句(`parse_node.h`)

事务控制语句不经过 bison 语法树,而是直接返回:
- `TxnBegin`、`TxnCommit`、`TxnAbort`(ROLLBACK)
- `LoadStmt`(加载 CSV 数据)
- `ExitStmt`、`HelpStmt`、`ShowTablesStmt`、`DescTableStmt`

---

## Parser 接口

```cpp
class Parser {
public:
    std::shared_ptr<ast::TreeNode> do_str_parse(std::string s);
    std::shared_ptr<ast::TreeNode> do_file_parse(std::string file);
};
```

`do_str_parse` 返回 AST 根节点;如果是控制语句(BEGIN/COMMIT 等),则返回 `nullptr`,解析器内部通过全局变量把控制信息传出。

---

## 词法分析(flex,`lex.l`)

| Token | 匹配规则 |
|---|---|
| `SELECT`/`FROM`/`WHERE`/... | 关键字(不区分大小写) |
| `[a-zA-Z_][a-zA-Z0-9_]*` | 标识符或关键字 |
| `[0-9]+` | 整数字面量 |
| `[0-9]+\.[0-9]+` | 浮点字面量 |
| `'[^']*'` | 字符串字面量 |
| `>=`/`<=`/`<>`/`>`/`<`/`=` | 比较运算符 |
| 空白/注释 | 跳过 |

---

## 语法分析(bison,`yacc.y`)

bison 依据 SQL 文法规则把 token 流归约成 AST 节点。核心规则:

```yacc
stmt ::= select_stmt
       | insert_stmt
       | delete_stmt
       | ...

select_stmt ::= SELECT col_list FROM tab_list opt_where opt_group_by opt_having opt_order_by

col_list ::= '*' | col_name (',' col_name)*

opt_where ::= /* empty */ | WHERE condition_list

condition_list ::= condition (AND condition)*
```

---

## 实现状态

| 组件 | 状态 | 说明 |
|---|---|---|
| `lex.l` / `lex.yy.cpp` | ✅ 框架提供 | flex 生成 |
| `yacc.y` / `yacc.tab.cpp` | ✅ 框架提供 | bison 生成 |
| AST 节点 | ✅ 框架提供 | 无需实验修改 |

> 上一篇:[07-system-manager](07-system-manager.md)
> 下一篇:[09-analyzer](09-analyzer.md) — 分析层。
