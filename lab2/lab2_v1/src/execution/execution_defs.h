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
#include <cmath>
#include <cstring>
#include <vector>

#include "common/common.h"
#include "defs.h"
#include "errors.h"
#include "system/sm_meta.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int lval = *reinterpret_cast<const int *>(lhs);
        int rval = *reinterpret_cast<const int *>(rhs);
        return (lval > rval) - (lval < rval);
    }
    if (type == TYPE_FLOAT) {
        float lval = *reinterpret_cast<const float *>(lhs);
        float rval = *reinterpret_cast<const float *>(rhs);
        if (std::fabs(lval - rval) <= 1e-6) {
            return 0;
        }
        return (lval > rval) - (lval < rval);
    }
    return std::strncmp(lhs, rhs, len);
}

inline bool compare_result(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

inline std::vector<ColMeta>::const_iterator find_col(const std::vector<ColMeta> &cols, const TabCol &target) {
    auto it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (it == cols.end()) {
        throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
    }
    return it;
}

inline bool eval_condition(const std::vector<ColMeta> &cols, const RmRecord *rec, const Condition &cond) {
    auto lhs_col = find_col(cols, cond.lhs_col);
    const char *lhs = rec->data + lhs_col->offset;
    const char *rhs = nullptr;
    ColType rhs_type;
    int rhs_len = lhs_col->len;
    if (cond.is_rhs_val) {
        rhs = cond.rhs_val.raw->data;
        rhs_type = cond.rhs_val.type;
    } else {
        auto rhs_col = find_col(cols, cond.rhs_col);
        rhs = rec->data + rhs_col->offset;
        rhs_type = rhs_col->type;
        rhs_len = rhs_col->len;
    }
    if (lhs_col->type != rhs_type) {
        bool lhs_numeric = lhs_col->type == TYPE_INT || lhs_col->type == TYPE_FLOAT;
        bool rhs_numeric = rhs_type == TYPE_INT || rhs_type == TYPE_FLOAT;
        if (lhs_numeric && rhs_numeric) {
            float lhs_float = lhs_col->type == TYPE_INT ? static_cast<float>(*reinterpret_cast<const int *>(lhs))
                                                        : *reinterpret_cast<const float *>(lhs);
            float rhs_float = rhs_type == TYPE_INT ? static_cast<float>(*reinterpret_cast<const int *>(rhs))
                                                   : *reinterpret_cast<const float *>(rhs);
            int cmp = 0;
            if (std::fabs(lhs_float - rhs_float) > 1e-6) {
                cmp = lhs_float < rhs_float ? -1 : 1;
            }
            return compare_result(cmp, cond.op);
        }
        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_type));
    }
    return compare_result(compare_raw_value(lhs, rhs, lhs_col->type, std::min(lhs_col->len, rhs_len)), cond.op);
}

inline bool eval_conditions(const std::vector<ColMeta> &cols, const RmRecord *rec,
                            const std::vector<Condition> &conds) {
    for (const auto &cond : conds) {
        if (!eval_condition(cols, rec, cond)) {
            return false;
        }
    }
    return true;
}
