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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;

        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };
        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    bool eval_cond(const char *rec_data, const Condition &cond) {
        auto lhs_col = get_col(cols_, cond.lhs_col);
        char *lhs_buf = const_cast<char *>(rec_data) + lhs_col->offset;
        if (cond.is_rhs_val) {
            char *rhs_buf = cond.rhs_val.raw->data;
            return compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op, lhs_col->len);
        } else {
            auto rhs_col = get_col(cols_, cond.rhs_col);
            char *rhs_buf = const_cast<char *>(rec_data) + rhs_col->offset;
            return compare_value(lhs_buf, rhs_buf, lhs_col->type, cond.op, lhs_col->len);
        }
    }

    bool compare_value(const char *lhs, const char *rhs, ColType type, CompOp op, int len = 0) {
        int cmp = 0;
        if (type == TYPE_INT) {
            int l = *(int *)lhs, r = *(int *)rhs;
            cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
        } else if (type == TYPE_DATETIME) {
            long long l = *(long long *)lhs, r = *(long long *)rhs;
            cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
        } else if (type == TYPE_FLOAT) {
            float l = *(float *)lhs, r = *(float *)rhs;
            cmp = (l < r) ? -1 : ((l > r) ? 1 : 0);
        } else if (type == TYPE_STRING) {
            cmp = strncmp(lhs, rhs, len);
        }
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

    bool is_satisfied(const char *rec_data) {
        for (auto &cond : fed_conds_)
            if (!eval_cond(rec_data, cond)) return false;
        return true;
    }

    void beginTuple() override {
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        auto *ih = sm_manager_->ihs_.at(ix_name).get();
        // Build key for lower bound from EQ conditions
        char key_buf[256] = {0};
        Iid lower = ih->leaf_begin();
        for (auto &cond : fed_conds_) {
            if (cond.is_rhs_val && cond.op == OP_EQ) {
                auto col_meta = get_col(cols_, cond.lhs_col);
                memcpy(key_buf, cond.rhs_val.raw->data, col_meta->len);
                lower = ih->lower_bound(key_buf);
                break;
            }
        }
        Iid upper = ih->leaf_end();
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        // Skip to first satisfying record
        while (!scan_->is_end()) {
            Rid r = scan_->rid();
            auto rec = fh_->get_record(r, context_);
            if (is_satisfied(rec->data)) { rid_ = r; break; }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            Rid r = scan_->rid();
            auto rec = fh_->get_record(r, context_);
            if (is_satisfied(rec->data)) {
                rid_ = r;
                break;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    bool is_end() const override { return scan_->is_end(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }
};