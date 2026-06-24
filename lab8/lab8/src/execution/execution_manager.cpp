/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"
#include <cstdio>
#include <iomanip>
#include <sstream>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

namespace {
bool is_float_result(ast::AggType agg_type, ColType col_type) {
    return col_type == TYPE_FLOAT && (agg_type == ast::AGG_SUM || agg_type == ast::AGG_MAX || agg_type == ast::AGG_MIN);
}

std::string format_float(double value) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(6) << value;
    return os.str();
}

std::vector<ColMeta>::const_iterator find_col(const std::vector<ColMeta> &cols, const TabCol &target) {
    auto pos = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
        return col.tab_name == target.tab_name && col.name == target.col_name;
    });
    if (pos == cols.end()) {
        throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
    }
    return pos;
}
}

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, txn_id_t *txn_id, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                memcpy(context->data_send_ + *(context->offset_), help_info, strlen(help_info));
                *(context->offset_) = strlen(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // 显示开启一个事务
                context->txn_->set_txn_mode(true);
                break;
            }  
            case T_Transaction_commit:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->commit(context->txn_, context->log_mgr_);
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_rollback:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                *txn_id = INVALID_TXN_ID;
                break;
            }    
            case T_Transaction_abort:
            {
                context->txn_ = txn_mgr_->get_transaction(*txn_id);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                *txn_id = INVALID_TXN_ID;
                break;
            }     
            default:
                throw InternalError("Unexpected field type");
                break;                        
        }

    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols, 
                            Context *context, std::vector<std::shared_ptr<ast::Col>> aggregate_cols) {
    std::vector<std::string> captions;
    captions.reserve(aggregate_cols.empty() ? sel_cols.size() : aggregate_cols.size());
    if (aggregate_cols.empty()) {
        for (auto &sel_col : sel_cols) {
            captions.push_back(sel_col.col_name);
        }
    } else {
        for (auto &agg_col : aggregate_cols) {
            captions.push_back(agg_col->alias);
        }
    }

    // Print header into buffer
    RecordPrinter rec_printer(captions.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);
    // print header into file
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "|";
    for(int i = 0; i < captions.size(); ++i) {
        outfile << " " << captions[i] << " |";
    }
    outfile << "\n";

    if (!aggregate_cols.empty()) {
        std::vector<long long> int_results(aggregate_cols.size(), 0);
        std::vector<double> float_results(aggregate_cols.size(), 0);
        std::vector<std::string> string_results(aggregate_cols.size());
        std::vector<ColType> result_types(aggregate_cols.size(), TYPE_INT);
        std::vector<bool> initialized(aggregate_cols.size(), false);
        size_t num_rec = 0;
        auto &cols = executorTreeRoot->cols();
        for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
            auto tuple = executorTreeRoot->Next();
            num_rec++;
            for (size_t i = 0; i < aggregate_cols.size(); i++) {
                auto &agg = aggregate_cols[i];
                if (agg->agg_type == ast::AGG_COUNT) {
                    int_results[i]++;
                    result_types[i] = TYPE_INT;
                    initialized[i] = true;
                    continue;
                }
                auto col_it = find_col(cols, {.tab_name = agg->tab_name, .col_name = agg->col_name});
                char *rec_buf = tuple->data + col_it->offset;
                result_types[i] = is_float_result(agg->agg_type, col_it->type) ? TYPE_FLOAT : col_it->type;
                if (col_it->type == TYPE_INT) {
                    int val = *(int *)rec_buf;
                    if (!initialized[i] || agg->agg_type == ast::AGG_SUM) int_results[i] += val;
                    if (!initialized[i] || agg->agg_type == ast::AGG_MAX) int_results[i] = std::max(int_results[i], (long long)val);
                    if (!initialized[i] || agg->agg_type == ast::AGG_MIN) int_results[i] = std::min(int_results[i], (long long)val);
                } else if (col_it->type == TYPE_BIGINT || col_it->type == TYPE_DATETIME) {
                    long long val = *(long long *)rec_buf;
                    if (!initialized[i] || agg->agg_type == ast::AGG_SUM) int_results[i] += val;
                    if (!initialized[i] || agg->agg_type == ast::AGG_MAX) int_results[i] = std::max(int_results[i], val);
                    if (!initialized[i] || agg->agg_type == ast::AGG_MIN) int_results[i] = std::min(int_results[i], val);
                } else if (col_it->type == TYPE_FLOAT) {
                    double val = *(float *)rec_buf;
                    if (!initialized[i] || agg->agg_type == ast::AGG_SUM) float_results[i] += val;
                    if (!initialized[i] || agg->agg_type == ast::AGG_MAX) float_results[i] = std::max(float_results[i], val);
                    if (!initialized[i] || agg->agg_type == ast::AGG_MIN) float_results[i] = std::min(float_results[i], val);
                } else if (col_it->type == TYPE_STRING) {
                    std::string val(rec_buf, col_it->len);
                    val.resize(strlen(val.c_str()));
                    if (!initialized[i] || (agg->agg_type == ast::AGG_MAX && val > string_results[i]) ||
                        (agg->agg_type == ast::AGG_MIN && val < string_results[i])) {
                        string_results[i] = val;
                    }
                }
                initialized[i] = true;
            }
        }
        std::vector<std::string> columns;
        for (size_t i = 0; i < aggregate_cols.size(); i++) {
            if (result_types[i] == TYPE_FLOAT) {
                columns.push_back(format_float(float_results[i]));
            } else if (result_types[i] == TYPE_STRING) {
                columns.push_back(string_results[i]);
            } else {
                columns.push_back(std::to_string(int_results[i]));
            }
        }
        rec_printer.print_record(columns, context);
        outfile << "|";
        for (int i = 0; i < columns.size(); ++i) {
            outfile << " " << columns[i] << " |";
        }
        outfile << "\n";
        outfile.close();
        rec_printer.print_separator(context);
        RecordPrinter::print_record_count(num_rec == 0 ? 0 : 1, context);
        return;
    }

    // Print records
    size_t num_rec = 0;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        auto Tuple = executorTreeRoot->Next();
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            char *rec_buf = Tuple->data + col.offset;
            if (col.type == TYPE_INT) {
                col_str = std::to_string(*(int *)rec_buf);
            } else if (col.type == TYPE_BIGINT) {
                col_str = std::to_string(*(long long *)rec_buf);
            } else if (col.type == TYPE_FLOAT) {
                col_str = std::to_string(*(float *)rec_buf);
            } else if (col.type == TYPE_DATETIME) {
                long long val = *(long long *)rec_buf;
                int year=(int)(val/10000000000LL);val%=10000000000LL;
                int month=(int)(val/100000000LL);val%=100000000LL;
                int day=(int)(val/1000000LL);val%=1000000LL;
                int hour=(int)(val/10000LL);val%=10000LL;
                int minute=(int)(val/100);int second=(int)(val%100);
                char buf[32];
                snprintf(buf,sizeof(buf),"%04d-%02d-%02d %02d:%02d:%02d",year,month,day,hour,minute,second);
                col_str = std::string(buf);
            } else if (col.type == TYPE_STRING) {
                col_str = std::string((char *)rec_buf, col.len);
                col_str.resize(strlen(col_str.c_str()));
            }
            columns.push_back(col_str);
        }
        // print record into buffer
        rec_printer.print_record(columns, context);
        // print record into file
        outfile << "|";
        for(int i = 0; i < columns.size(); ++i) {
            outfile << " " << columns[i] << " |";
        }
        outfile << "\n";
        num_rec++;
    }
    outfile.close();
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(num_rec, context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
