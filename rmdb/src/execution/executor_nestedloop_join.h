/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    // 比较两个原始值
    bool compare_value(const char *lhs, const char *rhs, ColType type, CompOp op) {
        int cmp_result = 0;
        if (type == TYPE_INT) {
            int l = *(int *)lhs;
            int r = *(int *)rhs;
            if (l < r) cmp_result = -1;
            else if (l > r) cmp_result = 1;
            else cmp_result = 0;
        } else if (type == TYPE_FLOAT) {
            float l = *(float *)lhs;
            float r = *(float *)rhs;
            if (l < r) cmp_result = -1;
            else if (l > r) cmp_result = 1;
            else cmp_result = 0;
        } else if (type == TYPE_STRING) {
            cmp_result = strcmp(lhs, rhs);
        }
        switch (op) {
            case OP_EQ: return cmp_result == 0;
            case OP_NE: return cmp_result != 0;
            case OP_LT: return cmp_result < 0;
            case OP_GT: return cmp_result > 0;
            case OP_LE: return cmp_result <= 0;
            case OP_GE: return cmp_result >= 0;
            default: return false;
        }
    }

    // 检查拼接后的记录是否满足所有 join 条件
    bool is_satisfied(const char *rec_data) {
        for (auto &cond : fed_conds_) {
            auto lhs_col = get_col(cols_, cond.lhs_col);
            char *lhs_buf = const_cast<char *>(rec_data) + lhs_col->offset;
            
            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
                if (!compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op)) {
                    return false;
                }
            } else {
                auto rhs_col = get_col(cols_, cond.rhs_col);
                char *rhs_buf = const_cast<char *>(rec_data) + rhs_col->offset;
                if (!compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op)) {
                    return false;
                }
            }
        }
        return true;
    }

    // 找到下一条满足 join 条件的记录对
    void find_next_valid() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                // 拼接左右记录检查条件
                auto left_rec = left_->Next();
                auto right_rec = right_->Next();
                std::unique_ptr<RmRecord> joined = std::make_unique<RmRecord>(len_);
                memcpy(joined->data, left_rec->data, left_->tupleLen());
                memcpy(joined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
                
                if (is_satisfied(joined->data)) {
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            right_->beginTuple();
        }
    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        if (!left_->is_end() && !right_->is_end()) {
            // 检查第一对记录是否满足条件
            auto left_rec = left_->Next();
            auto right_rec = right_->Next();
            std::unique_ptr<RmRecord> joined = std::make_unique<RmRecord>(len_);
            memcpy(joined->data, left_rec->data, left_->tupleLen());
            memcpy(joined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
            if (!is_satisfied(joined->data)) {
                find_next_valid();
            }
        }
    }

    void nextTuple() override {
        right_->nextTuple();
        find_next_valid();
    }

    std::unique_ptr<RmRecord> Next() override {
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();
        std::unique_ptr<RmRecord> joined = std::make_unique<RmRecord>(len_);
        memcpy(joined->data, left_rec->data, left_->tupleLen());
        memcpy(joined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return joined;
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return left_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};