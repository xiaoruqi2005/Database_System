/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "analyze.h"
#include <cstdint>
#include <cstdio>
#include <string>

static bool is_numeric_type(ColType type) {
    return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
}

static long long parse_datetime(const std::string &s) {
    if (s.length() != 19) throw IncompatibleTypeError("DATETIME", "invalid format");
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':')
        throw IncompatibleTypeError("DATETIME", "invalid format");
    for (int i = 0; i < 19; i++) {
        if (i==4||i==7||i==10||i==13||i==16) continue;
        if (s[i]<'0'||s[i]>'9') throw IncompatibleTypeError("DATETIME", "invalid format");
    }
    int year=std::stoi(s.substr(0,4)),month=std::stoi(s.substr(5,2)),day=std::stoi(s.substr(8,2));
    int hour=std::stoi(s.substr(11,2)),minute=std::stoi(s.substr(14,2)),second=std::stoi(s.substr(17,2));
    if (year<1000||year>9999) throw IncompatibleTypeError("DATETIME", "year");
    if (month<1||month>12) throw IncompatibleTypeError("DATETIME", "month");
    static const int dim[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int md=dim[month-1];
    bool leap=(year%4==0&&year%100!=0)||(year%400==0);
    if(month==2&&leap) md=29;
    if (day<1||day>md) throw IncompatibleTypeError("DATETIME", "day");
    if (hour>23||minute>59||second>59) throw IncompatibleTypeError("DATETIME", "time");
    return (long long)year*10000000000LL+(long long)month*100000000LL+(long long)day*1000000LL+(long long)hour*10000LL+(long long)minute*100+(long long)second;
}

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse) {
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse)) {
        query->tables = std::move(x->tabs);
        for (auto &tab_name : query->tables) {
            if (!sm_manager_->db_.is_table(tab_name)) throw TableNotFoundError(tab_name);
        }
        for (auto &sv_sel_col : x->cols) {
            query->cols.push_back({.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name});
        }
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (query->cols.empty()) {
            for (auto &col : all_cols)
                query->cols.push_back({.tab_name = col.tab_name, .col_name = col.name});
        } else {
            for (auto &sel_col : query->cols) sel_col = check_column(all_cols, sel_col);
        }
        // Process aggregate expressions if present
        for (auto &sv_agg : x->aggs) {
            AggSel agg_sel;
            agg_sel.agg_type = sv_agg->agg_type;
            agg_sel.is_count_star = sv_agg->is_count_star;
            if (agg_sel.is_count_star) {
                agg_sel.agg_col_type = TYPE_INT;
            } else {
                TabCol target = {.tab_name = sv_agg->col->tab_name, .col_name = sv_agg->col->col_name};
                target = check_column(all_cols, target);
                agg_sel.col = target;
                TabMeta &tab = sm_manager_->db_.get_table(target.tab_name);
                auto col_meta = tab.get_col(target.col_name);
                ColType col_type = col_meta->type;
                if (sv_agg->agg_type == ast::AGG_COUNT) {
                    agg_sel.agg_col_type = TYPE_INT;
                } else if (sv_agg->agg_type == ast::AGG_SUM) {
                    if (!is_numeric_type(col_type))
                        throw IncompatibleTypeError(coltype2str(col_type), "numeric");
                    agg_sel.agg_col_type = col_type;
                } else {
                    agg_sel.agg_col_type = col_type;
                }
            }
            if (!sv_agg->alias.empty()) {
                agg_sel.alias = sv_agg->alias;
            } else {
                static const char *agg_names[] = {"SUM", "MAX", "MIN", "COUNT"};
                if (agg_sel.is_count_star) {
                    agg_sel.alias = std::string("COUNT(*)");
                } else {
                    agg_sel.alias = std::string(agg_names[sv_agg->agg_type]) + "(" + sv_agg->col->col_name + ")";
                }
            }
            query->agg_sels.push_back(agg_sel);
        }
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
        query->tables = {x->tab_name};
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        for (auto &sv_set_clause : x->set_clauses) {
            SetClause set_clause;
            set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            set_clause.rhs = convert_sv_value(sv_set_clause->val);
            auto col = tab.get_col(sv_set_clause->col_name);
            bool set_compatible = (col->type == set_clause.rhs.type) ||
                (is_numeric_type(col->type) && is_numeric_type(set_clause.rhs.type)) ||
                (col->type == TYPE_DATETIME && set_clause.rhs.type == TYPE_STRING);
            if (!set_compatible) throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
            if (col->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_INT)
                set_clause.rhs.set_float(static_cast<float>(set_clause.rhs.int_val));
            else if (col->type == TYPE_FLOAT && set_clause.rhs.type == TYPE_BIGINT)
                set_clause.rhs.set_float(static_cast<float>(set_clause.rhs.bigint_val));
            else if (col->type == TYPE_INT && set_clause.rhs.type == TYPE_FLOAT)
                set_clause.rhs.set_int(static_cast<int>(set_clause.rhs.float_val));
            else if (col->type == TYPE_INT && set_clause.rhs.type == TYPE_BIGINT) {
                long long bv = set_clause.rhs.bigint_val;
                if (bv > INT32_MAX || bv < INT32_MIN)
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set_clause.rhs.type));
                set_clause.rhs.set_int(static_cast<int>(bv));
            } else if (col->type == TYPE_BIGINT && set_clause.rhs.type == TYPE_INT)
                set_clause.rhs.set_bigint(static_cast<long long>(set_clause.rhs.int_val));
            else if (col->type == TYPE_BIGINT && set_clause.rhs.type == TYPE_FLOAT)
                set_clause.rhs.set_bigint(static_cast<long long>(set_clause.rhs.float_val));
            else if (col->type == TYPE_DATETIME && set_clause.rhs.type == TYPE_STRING)
                set_clause.rhs.set_datetime(parse_datetime(set_clause.rhs.str_val));
            set_clause.rhs.init_raw(col->len);
            query->set_clauses.push_back(set_clause);
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        for (auto &sv_val : x->vals) query->values.push_back(convert_sv_value(sv_val));
    }
    query->parse = std::move(parse);
    return query;
}

TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) throw AmbiguousColumnError(target.col_name);
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) throw ColumnNotFoundError(target.col_name);
        target.tab_name = tab_name;
    } else {
        bool found = false;
        for (auto &col : all_cols)
            if (col.tab_name == target.tab_name && col.name == target.col_name) { found = true; break; }
        if (!found) throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    for (auto &cond : conds) {
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) cond.rhs_col = check_column(all_cols, cond.rhs_col);
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            ColType val_type = cond.rhs_val.type;
            if (val_type == TYPE_INT && lhs_type == TYPE_FLOAT)
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            else if (val_type == TYPE_FLOAT && lhs_type == TYPE_INT)
                cond.rhs_val.set_int(static_cast<int>(cond.rhs_val.float_val));
            else if (val_type == TYPE_INT && lhs_type == TYPE_BIGINT)
                cond.rhs_val.set_bigint(static_cast<long long>(cond.rhs_val.int_val));
            else if (val_type == TYPE_BIGINT && lhs_type == TYPE_INT) {
                long long bv = cond.rhs_val.bigint_val;
                if (bv > INT32_MAX || bv < INT32_MIN)
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(val_type));
                cond.rhs_val.set_int(static_cast<int>(bv));
            } else if (val_type == TYPE_BIGINT && lhs_type == TYPE_FLOAT)
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.bigint_val));
            else if (val_type == TYPE_FLOAT && lhs_type == TYPE_BIGINT)
                cond.rhs_val.set_bigint(static_cast<long long>(cond.rhs_val.float_val));
            else if (val_type == TYPE_STRING && lhs_type == TYPE_DATETIME)
                cond.rhs_val.set_datetime(parse_datetime(cond.rhs_val.str_val));
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        bool compatible = (lhs_type == rhs_type) ||
            (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) ||
            (lhs_type == TYPE_DATETIME && rhs_type == TYPE_STRING) ||
            (lhs_type == TYPE_STRING && rhs_type == TYPE_DATETIME);
        if (!compatible) throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
    }
}

Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        if (!int_lit->raw_str.empty()) {
            std::string s = int_lit->raw_str;
            bool negative = false;
            size_t start = 0;
            if (!s.empty() && s[0] == '+') {
                start = 1;
            } else if (!s.empty() && s[0] == '-') {
                negative = true;
                start = 1;
            }
            while (start < s.size() - 1 && s[start] == '0') start++;
            std::string digits = s.substr(start);
            const std::string max_str = "9223372036854775807";
            const std::string min_str = "9223372036854775808";
            auto cmp_abs = [](const std::string &a, const std::string &b) {
                if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
                return a.compare(b);
            };
            if (negative) {
                if (cmp_abs(digits, min_str) > 0) throw IncompatibleTypeError("BIGINT", "overflow");
            } else {
                if (cmp_abs(digits, max_str) > 0) throw IncompatibleTypeError("BIGINT", "overflow");
            }
        }
        long long v = int_lit->val;
        if (v > INT32_MAX || v < INT32_MIN)
            val.set_bigint(v);
        else
            val.set_int(static_cast<int>(v));
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
