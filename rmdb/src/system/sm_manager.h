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

#include <map>
#include <mutex>
#include <optional>

#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "sm_defs.h"
#include "sm_meta.h"
#include "common/common.h"
#include "common/context.h"

class Context;

struct ColDef {
    std::string name;  // Column name
    ColType type;      // Type of column
    int len;           // Length of column
};

/* 系统管理器，负责元数据管理和DDL语句的执行 */
class SmManager {
   public:
    DbMeta db_;             // 当前打开的数据库的元数据
    std::unordered_map<std::string, std::unique_ptr<RmFileHandle>> fhs_;    // file name -> record file handle, 当前数据库中每张表的数据文件
    std::unordered_map<std::string, std::unique_ptr<IxIndexHandle>> ihs_;   // file name -> index file handle, 当前数据库中每个索引的文件
    std::unordered_map<std::string, std::vector<std::map<std::string, std::vector<Rid>>>> memory_indexes_;
   private:
    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    RmManager* rm_manager_;
    IxManager* ix_manager_;
    std::recursive_mutex index_latch_;

   public:
    SmManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, RmManager* rm_manager,
              IxManager* ix_manager)
        : disk_manager_(disk_manager),
          buffer_pool_manager_(buffer_pool_manager),
          rm_manager_(rm_manager),
          ix_manager_(ix_manager) {}

    ~SmManager() {}

    BufferPoolManager* get_bpm() { return buffer_pool_manager_; }

    RmManager* get_rm_manager() { return rm_manager_; }  

    IxManager* get_ix_manager() { return ix_manager_; }  

    bool is_dir(const std::string& db_name);

    void create_db(const std::string& db_name);

    void drop_db(const std::string& db_name);

    void open_db(const std::string& db_name);

    void close_db();

    void flush_meta();

    void show_tables(Context* context);

    void desc_table(const std::string& tab_name, Context* context);

    void create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context);

    void drop_table(const std::string& tab_name, Context* context);

    void create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);

    void drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);
    
    void drop_index(const std::string& tab_name, const std::vector<ColMeta>& col_names, Context* context);

    void show_index(const std::string& tab_name, Context* context);

    std::string get_index_name(const std::string& tab_name, const std::vector<std::string>& col_names);
    std::string get_index_name(const IndexMeta& index);
    std::string build_index_key(const IndexMeta& index, const char* rec_data, size_t prefix_cols = 0);
    void rebuild_memory_index(const std::string& tab_name, const IndexMeta& index);
    void rebuild_all_memory_indexes();
    void check_unique_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid* self = nullptr);
    void insert_into_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid& rid);
    void delete_from_memory_indexes(const std::string& tab_name, const char* rec_data, const Rid* rid = nullptr);
    void update_memory_indexes(const std::string& tab_name, const char* old_data, const char* new_data, const Rid& rid);
    std::vector<Rid> scan_memory_index(const std::string& tab_name, const std::vector<std::string>& col_names,
                                       const std::vector<Condition>& conds);
};
