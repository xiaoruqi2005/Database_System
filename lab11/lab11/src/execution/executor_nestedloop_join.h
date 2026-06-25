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

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend = false;

    static constexpr size_t JOIN_BUFFER_SIZE = 64 * 1024 * 1024;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t left_block_pos_ = 0;
    std::unique_ptr<RmRecord> current_right_;
    std::unique_ptr<RmRecord> current_joined_;

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
        fed_conds_ = std::move(conds);
    }

    bool compare_value(const char *lhs, const char *rhs, ColType type, CompOp op, int len = 0) {
        return compare_value(lhs, rhs, type, type, op, len);
    }

    bool compare_value(const char *lhs, const char *rhs, ColType lhs_type, ColType rhs_type, CompOp op, int len = 0) {
        int cmp_result = 0;
        auto is_numeric_type = [](ColType type) {
            return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
        };
        auto read_numeric = [](const char *data, ColType type) {
            if (type == TYPE_INT) return static_cast<long double>(*(int *)data);
            if (type == TYPE_BIGINT) return static_cast<long double>(*(long long *)data);
            return static_cast<long double>(*(float *)data);
        };
        if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
            long double l = read_numeric(lhs, lhs_type);
            long double r = read_numeric(rhs, rhs_type);
            cmp_result = l < r ? -1 : (l > r ? 1 : 0);
        } else if (lhs_type == TYPE_DATETIME && rhs_type == TYPE_DATETIME) {
            long long l = *(long long *)lhs;
            long long r = *(long long *)rhs;
            cmp_result = l < r ? -1 : (l > r ? 1 : 0);
        } else if (lhs_type == TYPE_STRING && rhs_type == TYPE_STRING) {
            cmp_result = strncmp(lhs, rhs, len);
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

    bool is_satisfied(const char *rec_data) {
        for (auto &cond : fed_conds_) {
            auto lhs_col = get_col(cols_, cond.lhs_col);
            char *lhs_buf = const_cast<char *>(rec_data) + lhs_col->offset;

            if (cond.is_rhs_val) {
                char *rhs_buf = cond.rhs_val.raw->data;
                if (!compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op, lhs_col->len)) {
                    return false;
                }
            } else {
                auto rhs_col = get_col(cols_, cond.rhs_col);
                char *rhs_buf = const_cast<char *>(rec_data) + rhs_col->offset;
                if (!compare_value(lhs_buf, rhs_buf, lhs_col->type, rhs_col->type, cond.op, lhs_col->len)) {
                    return false;
                }
            }
        }
        return true;
    }

    void load_left_block() {
        left_block_.clear();
        left_block_pos_ = 0;
        size_t buffered_bytes = 0;
        const size_t tuple_len = std::max<size_t>(left_->tupleLen(), 1);
        const size_t max_tuples = std::max<size_t>(1, JOIN_BUFFER_SIZE / tuple_len);

        while (!left_->is_end() && left_block_.size() < max_tuples &&
               (left_block_.empty() || buffered_bytes + left_->tupleLen() <= JOIN_BUFFER_SIZE)) {
            auto rec = left_->Next();
            buffered_bytes += rec->size;
            left_block_.push_back(std::move(rec));
            left_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> join_records(const RmRecord *left_rec, const RmRecord *right_rec) {
        std::unique_ptr<RmRecord> joined = std::make_unique<RmRecord>(len_);
        memcpy(joined->data, left_rec->data, left_->tupleLen());
        memcpy(joined->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return joined;
    }

    void beginTuple() override {
        left_->beginTuple();
        load_left_block();
        if (!left_block_.empty()) {
            right_->beginTuple();
        }
        current_right_.reset();
        current_joined_.reset();
        isend = false;
        find_next_valid();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        current_joined_.reset();
        find_next_valid();
    }

    void find_next_valid() {
        while (!left_block_.empty()) {
            while (!right_->is_end()) {
                if (current_right_ == nullptr) {
                    current_right_ = right_->Next();
                }
                while (left_block_pos_ < left_block_.size()) {
                    auto joined = join_records(left_block_[left_block_pos_].get(), current_right_.get());
                    left_block_pos_++;
                    if (is_satisfied(joined->data)) {
                        current_joined_ = std::move(joined);
                        return;
                    }
                }
                right_->nextTuple();
                current_right_.reset();
                left_block_pos_ = 0;
            }

            load_left_block();
            if (!left_block_.empty()) {
                right_->beginTuple();
                current_right_.reset();
            }
        }
        isend = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (current_joined_ == nullptr) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*current_joined_);
    }

    Rid &rid() override { return _abstract_rid; }

    bool is_end() const override { return isend; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
