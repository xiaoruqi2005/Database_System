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

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

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
    
    // 先删除表上所有索引
    std::vector<std::vector<std::string>> idx_cols;
    for (auto &idx : tab.indexes) {
        std::vector<std::string> cols;
        for (auto &col : idx.cols) cols.push_back(col.name);
        idx_cols.push_back(cols);
    }
    for (auto &cols : idx_cols) {
        drop_index(tab_name, cols, context);
    }
    
    // 先正确关闭表的记录文件句柄（刷盘并关闭fd），再从 map 中移除
    auto it = fhs_.find(tab_name);
    if (it != fhs_.end()) {
        rm_manager_->close_file(it->second.get());
        fhs_.erase(it);
    }
    // 删除记录文件
    rm_manager_->destroy_file(tab_name);
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
    // 检查表是否存在
    TabMeta &tab = db_.get_table(tab_name);
    
    // 检查列是否合法
    std::vector<ColMeta> col_metas;
    int col_tot_len = 0;
    for (auto &col_name : col_names) {
        bool found = false;
        for (auto &col : tab.cols) {
            if (col.name == col_name) {
                col_metas.push_back(col);
                col_tot_len += col.len;
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(col_name);
        }
    }
    
    // 检查索引是否已存在
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }
    
    // 创建索引文件
    ix_manager_->create_index(tab_name, col_metas);
    
    // 构建索引元数据
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_tot_len = col_tot_len;
    index_meta.col_num = col_names.size();
    index_meta.cols = col_metas;
    
    // 打开索引文件并存储句柄
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, col_metas));
    
    // 在表元数据中添加索引
    tab.indexes.push_back(index_meta);
    
    // 设置列上的index标记
    for (auto &col_name : col_names) {
        for (auto &col : tab.cols) {
            if (col.name == col_name) {
                col.index = true;
                break;
            }
        }
    }
    
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // 检查表是否存在
    TabMeta &tab = db_.get_table(tab_name);
    
    // 构建索引名称
    std::string ix_name = ix_manager_->get_index_name(tab_name, col_names);
    
    // 从表元数据中删除index_meta
    bool found = false;
    for (auto it = tab.indexes.begin(); it != tab.indexes.end(); ++it) {
        if (it->cols.size() == col_names.size()) {
            size_t i = 0;
            for (; i < it->cols.size(); ++i) {
                if (it->cols[i].name != col_names[i]) break;
            }
            if (i == it->cols.size()) {
                tab.indexes.erase(it);
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    
    // 从ihs_中关闭并删除index handle
    auto ih_it = ihs_.find(ix_name);
    if (ih_it != ihs_.end()) {
        ix_manager_->close_index(ih_it->second.get());
        ihs_.erase(ih_it);
    }
    
    // 清除列上的index标记（如果该列不再被任何其他索引使用）
    for (auto &col_name : col_names) {
        bool still_indexed = false;
        for (auto &idx : tab.indexes) {
            for (auto &idx_col : idx.cols) {
                if (idx_col.name == col_name) {
                    still_indexed = true;
                    break;
                }
            }
            if (still_indexed) break;
        }
        if (!still_indexed) {
            for (auto &col : tab.cols) {
                if (col.name == col_name) {
                    col.index = false;
                    break;
                }
            }
        }
    }
    
    // 删除index文件
    ix_manager_->destroy_index(tab_name, col_names);
    
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    // 检查表是否存在
    TabMeta &tab = db_.get_table(tab_name);
    
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    
    RecordPrinter printer(3);
    printer.print_separator(context);
    
    for (auto &index : tab.indexes) {
        std::string col_str = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i > 0) col_str += ",";
            col_str += index.cols[i].name;
        }
        col_str += ")";
        printer.print_record({tab_name, "unique", col_str}, context);
        outfile << "| " << tab_name << " | unique | " << col_str << " |\n";
    }
    
    printer.print_separator(context);
    outfile.close();
}
