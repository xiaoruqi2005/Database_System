#!/bin/bash
# ============================================================
# RMDB Lab2 集成测试编排脚本
#
# 功能：
#   1. 检查编译产物
#   2. 清理旧的测试数据库
#   3. 在后台启动 RMDB 服务端
#   4. 等待服务端就绪
#   5. 运行 Python 集成测试
#   6. 关闭服务端进程
#   7. 清理并报告结果
#
# 用法: bash run_integration_test.sh
# ============================================================

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 路径配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RMDB_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$RMDB_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
TEST_DB="execution_test_db"
TEST_DB_PATH="$BUILD_DIR/$TEST_DB"
PYTHON_TEST="$SCRIPT_DIR/integration_test.py"
SERVER_PORT=8765
SERVER_PID=""

echo -e "${YELLOW}========================================${NC}"
echo -e "${YELLOW}RMDB Lab2 集成测试${NC}"
echo -e "${YELLOW}========================================${NC}"

# ─── 步骤1: 检查编译产物 ───
echo -e "\n${YELLOW}[1/5] 检查编译产物...${NC}"
if [ ! -f "$BIN_DIR/rmdb" ]; then
    echo -e "${RED}错误: 未找到 $BIN_DIR/rmdb${NC}"
    echo -e "${YELLOW}请先编译项目: cd rmdb && mkdir -p build && cd build && cmake .. && cmake --build . -j${NC}"
    exit 1
fi
echo -e "${GREEN}编译产物检查通过${NC}"

# ─── 步骤2: 清理旧的测试数据库 ───
echo -e "\n${YELLOW}[2/5] 清理旧的测试数据库...${NC}"
if [ -d "$TEST_DB_PATH" ]; then
    rm -rf "$TEST_DB_PATH"
    echo "已删除旧数据库目录: $TEST_DB_PATH"
fi
echo -e "${GREEN}清理完成${NC}"

# ─── 清理函数（用于退出时清理）───
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo -e "\n${YELLOW}正在关闭服务端进程 (PID: $SERVER_PID)...${NC}"
        kill "$SERVER_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    # 清理测试数据库
    if [ -d "$TEST_DB_PATH" ]; then
        rm -rf "$TEST_DB_PATH"
    fi
}
trap cleanup EXIT

# ─── 步骤3: 启动 RMDB 服务端 ───
echo -e "\n${YELLOW}[3/5] 启动 RMDB 服务端...${NC}"
cd "$BUILD_DIR"
SERVER_LOG="$BUILD_DIR/server_test.log"
"$BIN_DIR/rmdb" "$TEST_DB" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
echo "服务端启动，PID: $SERVER_PID"
echo "服务端日志: $SERVER_LOG"

# 等待服务端启动（给数据库恢复一些时间）
echo -n "等待服务端启动"
for i in $(seq 1 10); do
    echo -n "."
    sleep 0.5
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo ""
        echo -e "${RED}服务端进程已退出${NC}"
        exit 1
    fi
done
echo ""
echo -e "${GREEN}服务端已启动${NC}"

# ─── 步骤4: 运行集成测试 ───
echo -e "\n${YELLOW}[4/5] 运行集成测试...${NC}"
cd "$BUILD_DIR"
if python3 "$PYTHON_TEST" --host 127.0.0.1 --port $SERVER_PORT --output "$TEST_DB/output.txt"; then
    TEST_RESULT=0
else
    TEST_RESULT=1
fi

# ─── 步骤5: 关闭服务端并报告 ───
echo -e "\n${YELLOW}[5/5] 关闭服务端...${NC}"
kill "$SERVER_PID" 2>/dev/null || true
sleep 1
kill -9 "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo ""
if [ "$TEST_RESULT" -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}所有集成测试通过！${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}部分集成测试失败，请查看上方输出${NC}"
    echo -e "${RED}========================================${NC}"
fi

exit $TEST_RESULT
</path>
<task_progress>
- [x] 创建 `rmdb/.gitignore`
- [ ] 从 Git 暂存区移除 build 目录
- [x] 创建 Python Socket 集成测试客户端 `integration_test.py`
- [x] 创建 Shell 编排脚本 `run_integration_test.sh`
- [ ] 清理重建项目，确保编译通过
- [ ] 运行集成测试（5个测试点 + 错误用例）
- [ ] 修复发现的问题（如果有）
- [ ] 报告测试结果
</task_progress>
</write_to_file>