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
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RedoLogsInPage {
public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle *table_file_;
    std::vector<lsn_t> redo_logs_;
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, SmManager *sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
    }

    void analyze();
    void redo();
    void undo();

private:
    bool record_exists(RmFileHandle *fh, const Rid &rid);
    std::unique_ptr<LogRecord> deserialize_log_record(const char *data);

    LogBuffer buffer_;
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    SmManager *sm_manager_;
    std::vector<std::unique_ptr<LogRecord>> log_records_;
    std::unordered_set<txn_id_t> committed_txns_;
    std::unordered_set<txn_id_t> finished_txns_;
    std::unordered_set<txn_id_t> active_txns_;
};
