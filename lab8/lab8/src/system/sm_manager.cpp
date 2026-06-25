/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
#include "execution/executor_seq_scan.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    // 读取数据库元数据
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;
    // 打开所有表的记录文件
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
    }
    rebuild_all_memory_indexes();
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 刷新元数据
    flush_meta();
    // 关闭所有表的记录文件句柄
    fhs_.clear();
    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 检查表是否存在，不存在则抛出异常（会被上层捕获并输出 failure）
    TabMeta &tab = db_.get_table(tab_name);
    // 先正确关闭表的记录文件句柄（刷盘并关闭fd），再从 map 中移除
    auto it = fhs_.find(tab_name);
    if (it != fhs_.end()) {
        rm_manager_->close_file(it->second.get());
        fhs_.erase(it);
    }
    // 删除记录文件
    rm_manager_->destroy_file(tab_name);
    {
        std::lock_guard<std::recursive_mutex> lock(index_latch_);
        for (auto it = memory_indexes_.begin(); it != memory_indexes_.end();) {
            if (it->first.rfind(tab_name + ".", 0) == 0) it = memory_indexes_.erase(it);
            else ++it;
        }
    }
    // 从元数据中移除表
    db_.tabs_.erase(tab_name);
    // 刷新元数据
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) throw IndexExistsError(tab_name, col_names);
    IndexMeta index;
    index.tab_name = tab_name;
    index.col_num = static_cast<int>(col_names.size());
    index.col_tot_len = 0;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index.cols.push_back(*col);
        index.col_tot_len += col->len;
    }
    tab.indexes.push_back(index);
    for (auto &col_name : col_names) {
        tab.get_col(col_name)->index = true;
    }
    try {
        rebuild_memory_index(tab_name, index);
    } catch (...) {
        tab.indexes.pop_back();
        throw;
    }
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    auto index = tab.get_index_meta(col_names);
    {
        std::lock_guard<std::recursive_mutex> lock(index_latch_);
        memory_indexes_.erase(get_index_name(*index));
    }
    tab.indexes.erase(index);
    for (auto &col : tab.cols) {
        col.index = false;
        for (auto &idx : tab.indexes) {
            for (auto &idx_col : idx.cols) {
                if (idx_col.name == col.name) col.index = true;
            }
        }
    }
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) col_names.push_back(col.name);
    drop_index(tab_name, col_names, context);
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::fstream outfile("output.txt", std::ios::out | std::ios::app);
    RecordPrinter printer(3);
    printer.print_separator(context);
    for (auto &index : tab.indexes) {
        std::string cols = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i) cols += ",";
            cols += index.cols[i].name;
        }
        cols += ")";
        printer.print_record({tab_name, "unique", cols}, context);
        outfile << "| " << tab_name << " | unique | " << cols << " |\n";
    }
    printer.print_separator(context);
}

std::string SmManager::get_index_name(const std::string& tab_name, const std::vector<std::string>& col_names) {
    std::string name = tab_name + ".";
    for (size_t i = 0; i < col_names.size(); ++i) {
        if (i) name += "#";
        name += col_names[i];
    }
    return name;
}

std::string SmManager::get_index_name(const IndexMeta& index) {
    std::vector<std::string> col_names;
    for (auto &col : index.cols) col_names.push_back(col.name);
    return get_index_name(index.tab_name, col_names);
}

std::string SmManager::build_index_key(const IndexMeta& index, const char* rec_data, size_t prefix_cols) {
    std::string key;
    key.reserve(index.col_tot_len);
    if (prefix_cols == 0 || prefix_cols > index.cols.size()) prefix_cols = index.cols.size();
    for (size_t i = 0; i < prefix_cols; ++i) {
        auto &col = index.cols[i];
        key.append(rec_data + col.offset, col.len);
    }
    return key;
}

void SmManager::rebuild_memory_index(const std::string& tab_name, const IndexMeta& index) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    auto &prefix_maps = memory_indexes_[get_index_name(index)];
    prefix_maps.assign(index.cols.size(), {});
    auto fh = fhs_.at(tab_name).get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        auto rec = fh->get_record(scan.rid(), nullptr);
        std::string key = build_index_key(index, rec->data);
        if (!prefix_maps.empty() && prefix_maps.back().find(key) != prefix_maps.back().end()) {
            throw IndexExistsError(tab_name, std::vector<std::string>{});
        }
        for (size_t i = 0; i < index.cols.size(); ++i) {
            prefix_maps[i][build_index_key(index, rec->data, i + 1)].push_back(scan.rid());
        }
    }
}

void SmManager::rebuild_all_memory_indexes() {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    memory_indexes_.clear();
    for (auto &tab_pair : db_.tabs_) {
        for (auto &index : tab_pair.second.indexes) rebuild_memory_index(tab_pair.first, index);
    }
}

void SmManager::check_unique_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid* self) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto &prefix_maps = memory_indexes_[get_index_name(index)];
        auto key = build_index_key(index, rec_data);
        auto it = prefix_maps.empty() ? std::map<std::string, std::vector<Rid>>::iterator{} : prefix_maps.back().find(key);
        if (!prefix_maps.empty() && it != prefix_maps.back().end() &&
            (!self || it->second.size() != 1 || it->second[0] != *self)) {
            throw IndexExistsError(tab_name, std::vector<std::string>{});
        }
    }
}

void SmManager::insert_into_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid& rid) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto &prefix_maps = memory_indexes_[get_index_name(index)];
        if (prefix_maps.size() != index.cols.size()) prefix_maps.assign(index.cols.size(), {});
        for (size_t i = 0; i < index.cols.size(); ++i) {
            prefix_maps[i][build_index_key(index, rec_data, i + 1)].push_back(rid);
        }
    }
}

void SmManager::delete_from_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid* rid) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        auto &prefix_maps = memory_indexes_[get_index_name(index)];
        for (size_t i = 0; i < index.cols.size() && i < prefix_maps.size(); ++i) {
            auto key = build_index_key(index, rec_data, i + 1);
            auto it = prefix_maps[i].find(key);
            if (it == prefix_maps[i].end()) continue;
            auto &vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const Rid &stored_rid) {
                return rid == nullptr || stored_rid == *rid;
            }), vec.end());
            if (vec.empty()) prefix_maps[i].erase(it);
        }
    }
}

void SmManager::update_memory_indexes(const std::string& tab_name, const char* old_data, const char* new_data, const Rid& rid) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    delete_from_memory_indexes(tab_name, old_data, &rid);
    insert_into_memory_indexes(tab_name, new_data, rid);
}

std::vector<Rid> SmManager::scan_memory_index(const std::string& tab_name, const std::vector<std::string>& col_names,
                                              const std::vector<Condition>& conds) {
    std::lock_guard<std::recursive_mutex> lock(index_latch_);
    TabMeta &tab = db_.get_table(tab_name);
    IndexMeta index = *tab.get_index_meta(col_names);
    auto &prefix_maps = memory_indexes_[get_index_name(index)];
    auto fh = fhs_.at(tab_name).get();
    std::vector<Rid> rids;

    std::string prefix;
    size_t matched_eq_cols = 0;
    for (auto &col : index.cols) {
        const Condition *eq_cond = nullptr;
        for (auto &cond : conds) {
            if (cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.col_name == col.name) {
                eq_cond = &cond;
                break;
            }
        }
        if (!eq_cond) break;
        prefix.append(eq_cond->rhs_val.raw->data, col.len);
        ++matched_eq_cols;
    }

    auto scan_rid = [&](const Rid &rid) {
        auto rec = fh->get_record(rid, nullptr);
        SeqScanExecutor checker(this, tab_name, conds, nullptr);
        if (checker.is_satisfied(rec->data)) rids.push_back(rid);
    };

    if (matched_eq_cols > 0 && matched_eq_cols <= prefix_maps.size()) {
        auto &map = prefix_maps[matched_eq_cols - 1];
        auto it = map.find(prefix);
        if (it != map.end()) {
            for (auto &rid : it->second) scan_rid(rid);
        }
    } else {
        auto &map = prefix_maps.front();
        for (auto &entry : map) {
            for (auto &rid : entry.second) scan_rid(rid);
        }
    }
    return rids;
}
