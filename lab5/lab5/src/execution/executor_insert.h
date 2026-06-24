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
#include <cstdint>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            auto is_numeric_type = [](ColType type) {
                return type == TYPE_INT || type == TYPE_BIGINT || type == TYPE_FLOAT;
            };
            bool compatible = (col.type == val.type) ||
                (is_numeric_type(col.type) && is_numeric_type(val.type)) ||
                (col.type == TYPE_DATETIME && val.type == TYPE_STRING);
            if (!compatible) throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                val.set_float(static_cast<float>(val.int_val));
            } else if (col.type == TYPE_FLOAT && val.type == TYPE_BIGINT) {
                val.set_float(static_cast<float>(val.bigint_val));
            } else if (col.type == TYPE_INT && val.type == TYPE_FLOAT) {
                val.set_int(static_cast<int>(val.float_val));
            } else if (col.type == TYPE_INT && val.type == TYPE_BIGINT) {
                long long bv = val.bigint_val;
                if (bv > INT32_MAX || bv < INT32_MIN) {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
                val.set_int(static_cast<int>(bv));
            } else if (col.type == TYPE_BIGINT && val.type == TYPE_INT) {
                val.set_bigint(static_cast<long long>(val.int_val));
            } else if (col.type == TYPE_BIGINT && val.type == TYPE_FLOAT) {
                val.set_bigint(static_cast<long long>(val.float_val));
            } else if (col.type == TYPE_DATETIME && val.type == TYPE_STRING) {
                const std::string &s = val.str_val;
                if (s.length() != 19 || s[4] != 45 || s[7] != 45 || s[10] != 32 || s[13] != 58 || s[16] != 58)
                    throw IncompatibleTypeError("DATETIME", "invalid format");
                for (int ci = 0; ci < 19; ci++) {
                    if (ci==4||ci==7||ci==10||ci==13||ci==16) continue;
                    if (s[ci]<48||s[ci]>57) throw IncompatibleTypeError("DATETIME", "invalid format");
                }
                int year = std::stoi(s.substr(0,4)), month = std::stoi(s.substr(5,2)), day = std::stoi(s.substr(8,2));
                int hour = std::stoi(s.substr(11,2)), minute = std::stoi(s.substr(14,2)), second = std::stoi(s.substr(17,2));
                if (year<1000||year>9999) throw IncompatibleTypeError("DATETIME", "year");
                if (month<1||month>12) throw IncompatibleTypeError("DATETIME", "month");
                static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                int md = dim[month-1];
                bool leap = (year%4==0&&year%100!=0)||(year%400==0);
                if (month==2&&leap) md = 29;
                if (day<1||day>md) throw IncompatibleTypeError("DATETIME", "day");
                if (hour>23||minute>59||second>59) throw IncompatibleTypeError("DATETIME", "time");
                val.set_datetime((long long)year*10000000000LL+(long long)month*100000000LL+(long long)day*1000000LL+(long long)hour*10000LL+(long long)minute*100+(long long)second);
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        sm_manager_->check_unique_memory_indexes(tab_name_, rec.data);
        // Insert into record file
        rid_ = fh_->insert_record(rec.data, context_);
        sm_manager_->insert_into_memory_indexes(tab_name_, rec.data, rid_);
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
