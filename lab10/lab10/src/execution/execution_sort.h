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

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int limit_;
    std::vector<std::unique_ptr<RmRecord>> tuples_;
    size_t cursor_ = 0;
    bool materialized_ = false;

    int compare_field(const RmRecord *lhs, const RmRecord *rhs, const ColMeta &col) const {
        const char *l = lhs->data + col.offset;
        const char *r = rhs->data + col.offset;
        if (col.type == TYPE_INT) {
            int lv = *(int *)l, rv = *(int *)r;
            return (lv > rv) - (lv < rv);
        }
        if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            long long lv = *(long long *)l, rv = *(long long *)r;
            return (lv > rv) - (lv < rv);
        }
        if (col.type == TYPE_FLOAT) {
            float lv = *(float *)l, rv = *(float *)r;
            return (lv > rv) - (lv < rv);
        }
        int cmp = std::strncmp(l, r, col.len);
        return (cmp > 0) - (cmp < 0);
    }

    void materialize() {
        if (materialized_) return;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        if (!sort_cols_.empty()) {
            std::stable_sort(tuples_.begin(), tuples_.end(), [&](const auto &lhs, const auto &rhs) {
                for (size_t i = 0; i < sort_cols_.size(); i++) {
                    int cmp = compare_field(lhs.get(), rhs.get(), sort_cols_[i]);
                    if (cmp == 0) continue;
                    return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
                return false;
            });
        }
        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(static_cast<size_t>(limit_));
        }
        materialized_ = true;
        cursor_ = 0;
    }

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols,
                 const std::vector<bool> &is_desc, int limit) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        for (auto &sel_col : sel_cols) {
            sort_cols_.push_back(*get_col(cols_, sel_col));
        }
        is_desc_ = is_desc;
        limit_ = limit;
    }

    void beginTuple() override {
        materialize();
        cursor_ = 0;
    }

    void nextTuple() override {
        materialize();
        if (cursor_ < tuples_.size()) cursor_++;
    }

    std::unique_ptr<RmRecord> Next() override {
        materialize();
        if (is_end()) return nullptr;
        return std::make_unique<RmRecord>(*tuples_[cursor_]);
    }

    bool is_end() const override { return materialized_ && cursor_ >= tuples_.size(); }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};
