# 02 项目结构与构建系统

> 相关源文件:`CMakeLists.txt`、`src/CMakeLists.txt`、各子目录 `CMakeLists.txt`、`README.md`、`rmdb_client/`

## 一句话职责

本章描述 RMDB 的物理目录布局、CMake 构建层次、各模块库与可执行产物的依赖关系,以及如何编译运行。

## 目录树

```
lab1/lab1/                         # 项目根目录(可构建)
├── CMakeLists.txt                 # 顶层 CMake,定义项目名/标准/子目录
├── README.md                      # 项目说明
├── License                        # 木兰宽松许可证 v2
├── rmdb_client/                   # 压测客户端
│   ├── CMakeLists.txt
│   └── main.cpp
├── deps/                          # 第三方依赖(原仓库,lab1 中未列出树内)
│   └── googletest/                # GoogleTest 单元测试框架
└── src/                           # 核心源代码
    ├── CMakeLists.txt             # 汇总所有子模块
    ├── rmdb.cpp                   # 主程序入口(交互式 CLI)
    ├── portal.h                   # Portal 门面(串联 Parser→Exec)
    ├── unit_test.cpp              # GoogleTest 测试入口
    ├── record_printer.h           # 查询结果格式化打印
    ├── defs.h                     # 全局类型(Rid, ColType, RecScan)
    ├── errors.h                   # 异常体系
    ├── common/
    │   ├── config.h               # 全局常量与类型别名
    │   ├── context.h              # 运行时上下文 Context
    │   └── common.h               # 公共辅助宏/函数
    ├── storage/
    │   ├── page.h                 # Page/PageId 定义
    │   ├── disk_manager.{h,cpp}   # 磁盘管理
    │   └── buffer_pool_manager.{h,cpp}  # 缓冲池
    ├── replacer/
    │   ├── replacer.h             # 替换器抽象接口
    │   └── lru_replacer.{h,cpp}   # LRU 实现
    ├── record/
    │   ├── rm_defs.h              # 记录文件定义
    │   ├── rm_file_handle.{h,cpp} # 记录文件句柄
    │   ├── rm_manager.h           # 记录管理器
    │   ├── rm_scan.{h,cpp}        # 记录扫描
    │   └── bitmap.h               # 位图操作
    ├── index/
    │   ├── ix_defs.h              # B+树定义
    │   ├── ix_index_handle.{h,cpp}# B+树句柄
    │   ├── ix_manager.h           # 索引管理器
    │   └── ix_scan.{h,cpp}        # 索引扫描
    ├── system/
    │   ├── sm_defs.h              # 系统管理定义
    │   ├── sm_meta.h              # 元数据(TabMeta/ColMeta/DbMeta)
    │   └── sm_manager.{h,cpp}     # 系统管理器
    ├── parser/
    │   ├── parser.h               # Parser 封装
    │   ├── parser_defs.h          # yacc 接口
    │   ├── ast.{h,cpp}            # AST 节点定义
    │   ├── parse_node.h           # 控制类 AST 节点
    │   ├── lex.l / lex.yy.cpp     # flex 词法分析
    │   ├── yacc.y / yacc.tab.cpp  # bison 语法分析
    │   └── ast_printer.h          # AST 可视化
    ├── analyze/
    │   └── analyze.{h,cpp}        # 语义分析器
    ├── optimizer/
    │   ├── plan.h                 # Plan 节点定义
    │   ├── planner.{h,cpp}        # AST→Plan 转换
    │   └── optimizer.h            # 优化器入口
    ├── execution/
    │   ├── execution_manager.{h,cpp}  # 执行管理器
    │   ├── executor_abstract.h    # 执行算子基类
    │   ├── executor_seq_scan.h
    │   ├── executor_index_scan.h
    │   ├── executor_insert.h
    │   ├── executor_delete.h
    │   ├── executor_update.h
    │   ├── executor_projection.h
    │   ├── executor_nestedloop_join.h
    │   └── execution_sort.h       # 排序算子
    ├── transaction/
    │   ├── transaction.h          # 事务对象
    │   ├── transaction_manager.{h,cpp}
    │   ├── txn_defs.h             # 事务定义
    │   └── concurrency/
    │       └── lock_manager.{h,cpp}
    ├── recovery/
    │   ├── log_defs.h             # 日志定义
    │   ├── log_manager.{h,cpp}
    │   └── log_recovery.{h,cpp}
    └── test/
        └── performance_test/      # TPC-C 性能测试数据
            └── table_data/*.csv
```

## CMake 构建层次

```
顶层 CMakeLists.txt
 ├── project(rmdb C CXX)
 ├── set(CMAKE_CXX_STANDARD 17)
 ├── add_subdirectory(deps/googletest)   # 单元测试框架
 ├── add_subdirectory(src)
 │     ├── add_library(storage ...)      # storage + replacer
 │     ├── add_library(record ...)
 │     ├── add_library(index ...)
 │     ├── add_library(system ...)
 │     ├── add_library(parser ...)
 │     ├── add_library(analyze ...)
 │     ├── add_library(optimizer ...)
 │     ├── add_library(execution ...)
 │     ├── add_library(transaction ...)
 │     ├── add_library(recovery ...)
 │     ├── add_executable(rmdb rmdb.cpp)  # 主程序 CLI
 │     ├── add_executable(unit_test ...)  # GTest 单元测试
 │     └── target_link_libraries(rmdb PRIVATE <所有模块库>)
 └── add_subdirectory(rmdb_client)        # 压测客户端
```

各模块编译为独立的**静态库**,上层模块链接下层:

```
+-----------+   +-----------+   +-----------+
|  storage  |   | replacer  |   |  parser   |
+-----------+   +-----------+   +-----------+
      ^               ^               (flex/bison generated)
      |
+-----------+
|  record   |---+---+---+
+-----------+   |   |   |
      ^         |   |   |
      |         v   v   v
+-----------+ +-------+ +-----------+
|  index    | |system | | execution |
+-----------+ +-------+ +-----------+
                    ^         ^
                    |         |
              +-----------+ +-----------+
              | optimizer | | analyze   |
              +-----------+ +-----------+
                    ^         ^
                    +----+----+
                         |
                    +-----------+
                    |   rmdb    | (executable)
                    +-----------+
```

> 事务与恢复库同样链接进 `rmdb` 与 `unit_test`。

## 构建命令

```bash
cd lab1/lab1
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

### 可执行产物

| 产物 | 源文件 | 用途 |
|---|---|---|
| `rmdb` | `rmdb.cpp` | 交互式 SQL CLI,读入 SQL、输出结果到 `output.txt` |
| `unit_test` | `unit_test.cpp` | GoogleTest 单元测试 |
| `rmdb_client` | `rmdb_client/main.cpp` | TPC-C 压测客户端 |

### 运行单元测试

```bash
cd build
ctest --output-on-failure          # 或直接 ./unit_test
```

### 运行 RMDB CLI

```bash
./rmdb <db_name>                   # 打开/创建数据库目录,进入交互
```

RMDB 启动后读取标准输入的 SQL 语句,查询结果写入当前目录的 `output.txt`。

## 编译依赖

| 依赖 | 用途 |
|---|---|
| **flex** | 词法分析器生成(`lex.l` → `lex.yy.cpp`) |
| **bison** | 语法分析器生成(`yacc.y` → `yacc.tab.cpp`) |
| **readline** | CLI 交互式行编辑 |
| **googletest** | 单元测试(随仓库 `deps/` 提供) |
| **C++17** | `std::filesystem`、结构化绑定等 |

> 在 WSL/Ubuntu 下安装:`sudo apt install flex bison libreadline-dev cmake g++`。

## parser 的特殊性

`parser` 模块由 flex/bison 生成部分代码:
- `lex.l` → `lex.yy.cpp`(词法)
- `yacc.y` → `yacc.tab.cpp` / `yacc.tab.h`(语法)

仓库中同时保留了生成后的 `.cpp` 文件和原始 `.l` / `.y` 文件。CMake 优先编译生成后的 `.cpp`;若需重新生成,需手动调用 `flex` 和 `bison`。

## 实现状态

构建系统本身由框架提供,无需实验修改。各实验只需要在对应模块目录内实现 `TODO` 即可,无需改动 CMakeLists(除非新增源文件)。

> 上一篇:[01-architecture-overview](01-architecture-overview.md)
> 下一篇:[03-common-infrastructure](03-common-infrastructure.md) — 公共层详解。