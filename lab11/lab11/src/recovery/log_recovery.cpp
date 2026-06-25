/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <cstring>

bool RecoveryManager::record_exists(RmFileHandle *fh, const Rid &rid) {
    if (fh == nullptr) {
        return false;
    }
    try {
        return fh->is_record(rid);
    } catch (...) {
        return false;
    }
}

std::unique_ptr<LogRecord> RecoveryManager::deserialize_log_record(const char *data) {
    LogType type = *reinterpret_cast<const LogType *>(data + OFFSET_LOG_TYPE);
    std::unique_ptr<LogRecord> record;
    switch (type) {
        case LogType::UPDATE:
            record = std::make_unique<UpdateLogRecord>();
            break;
        case LogType::INSERT:
            record = std::make_unique<InsertLogRecord>();
            break;
        case LogType::DELETE:
            record = std::make_unique<DeleteLogRecord>();
            break;
        case LogType::begin:
            record = std::make_unique<BeginLogRecord>();
            break;
        case LogType::commit:
            record = std::make_unique<CommitLogRecord>();
            break;
        case LogType::ABORT:
            record = std::make_unique<AbortLogRecord>();
            break;
        default:
            return nullptr;
    }
    record->deserialize(data);
    return record;
}

void RecoveryManager::analyze() {
    log_records_.clear();
    committed_txns_.clear();
    finished_txns_.clear();
    active_txns_.clear();

    int offset = 0;
    while (true) {
        char header[LOG_HEADER_SIZE];
        int header_size = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (header_size <= 0) {
            break;
        }
        if (header_size < LOG_HEADER_SIZE) {
            break;
        }

        uint32_t log_tot_len = *reinterpret_cast<uint32_t *>(header + OFFSET_LOG_TOT_LEN);
        if (log_tot_len < LOG_HEADER_SIZE || log_tot_len > LOG_BUFFER_SIZE) {
            break;
        }

        std::vector<char> log_data(log_tot_len);
        memcpy(log_data.data(), header, LOG_HEADER_SIZE);
        int body_size = static_cast<int>(log_tot_len) - LOG_HEADER_SIZE;
        if (body_size > 0) {
            int read_size = disk_manager_->read_log(log_data.data() + LOG_HEADER_SIZE, body_size,
                                                    offset + LOG_HEADER_SIZE);
            if (read_size < body_size) {
                break;
            }
        }

        auto record = deserialize_log_record(log_data.data());
        if (record == nullptr) {
            break;
        }

        txn_id_t tid = record->log_tid_;
        switch (record->log_type_) {
            case LogType::begin:
                active_txns_.insert(tid);
                break;
            case LogType::commit:
                committed_txns_.insert(tid);
                finished_txns_.insert(tid);
                active_txns_.erase(tid);
                break;
            case LogType::ABORT:
                finished_txns_.insert(tid);
                active_txns_.erase(tid);
                break;
            case LogType::INSERT:
            case LogType::DELETE:
            case LogType::UPDATE:
                if (!finished_txns_.count(tid)) {
                    active_txns_.insert(tid);
                }
                break;
        }

        log_records_.push_back(std::move(record));
        offset += static_cast<int>(log_tot_len);
    }
}

void RecoveryManager::redo() {
    for (const auto &record_ptr : log_records_) {
        LogRecord *record = record_ptr.get();
        if (!committed_txns_.count(record->log_tid_)) {
            continue;
        }

        if (record->log_type_ == LogType::INSERT) {
            auto *insert_record = dynamic_cast<InsertLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(insert_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, insert_record->rid_)) {
                auto old_record = fh->get_record(insert_record->rid_, nullptr);
                fh->update_record(insert_record->rid_, insert_record->insert_value_.data, nullptr);
                sm_manager_->update_memory_indexes(insert_record->table_name_, old_record->data,
                                                   insert_record->insert_value_.data, insert_record->rid_);
            } else {
                fh->insert_record(insert_record->rid_, insert_record->insert_value_.data);
                sm_manager_->insert_into_memory_indexes(insert_record->table_name_, insert_record->insert_value_.data,
                                                        insert_record->rid_);
            }
        } else if (record->log_type_ == LogType::DELETE) {
            auto *delete_record = dynamic_cast<DeleteLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(delete_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, delete_record->rid_)) {
                auto old_record = fh->get_record(delete_record->rid_, nullptr);
                sm_manager_->delete_from_memory_indexes(delete_record->table_name_, old_record->data,
                                                        &delete_record->rid_);
                fh->delete_record(delete_record->rid_, nullptr);
            }
        } else if (record->log_type_ == LogType::UPDATE) {
            auto *update_record = dynamic_cast<UpdateLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(update_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, update_record->rid_)) {
                auto old_record = fh->get_record(update_record->rid_, nullptr);
                fh->update_record(update_record->rid_, update_record->new_value_.data, nullptr);
                sm_manager_->update_memory_indexes(update_record->table_name_, old_record->data,
                                                   update_record->new_value_.data, update_record->rid_);
            } else {
                fh->insert_record(update_record->rid_, update_record->new_value_.data);
                sm_manager_->insert_into_memory_indexes(update_record->table_name_, update_record->new_value_.data,
                                                        update_record->rid_);
            }
        }
    }
}

void RecoveryManager::undo() {
    for (auto it = log_records_.rbegin(); it != log_records_.rend(); ++it) {
        LogRecord *record = it->get();
        if (finished_txns_.count(record->log_tid_)) {
            continue;
        }

        if (record->log_type_ == LogType::INSERT) {
            auto *insert_record = dynamic_cast<InsertLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(insert_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, insert_record->rid_)) {
                auto old_record = fh->get_record(insert_record->rid_, nullptr);
                sm_manager_->delete_from_memory_indexes(insert_record->table_name_, old_record->data,
                                                        &insert_record->rid_);
                fh->delete_record(insert_record->rid_, nullptr);
            }
        } else if (record->log_type_ == LogType::DELETE) {
            auto *delete_record = dynamic_cast<DeleteLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(delete_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, delete_record->rid_)) {
                auto curr_record = fh->get_record(delete_record->rid_, nullptr);
                fh->update_record(delete_record->rid_, delete_record->delete_value_.data, nullptr);
                sm_manager_->update_memory_indexes(delete_record->table_name_, curr_record->data,
                                                   delete_record->delete_value_.data, delete_record->rid_);
            } else {
                fh->insert_record(delete_record->rid_, delete_record->delete_value_.data);
                sm_manager_->insert_into_memory_indexes(delete_record->table_name_, delete_record->delete_value_.data,
                                                        delete_record->rid_);
            }
        } else if (record->log_type_ == LogType::UPDATE) {
            auto *update_record = dynamic_cast<UpdateLogRecord *>(record);
            auto table_it = sm_manager_->fhs_.find(update_record->table_name_);
            if (table_it == sm_manager_->fhs_.end()) {
                continue;
            }
            RmFileHandle *fh = table_it->second.get();
            if (record_exists(fh, update_record->rid_)) {
                auto curr_record = fh->get_record(update_record->rid_, nullptr);
                fh->update_record(update_record->rid_, update_record->old_value_.data, nullptr);
                sm_manager_->update_memory_indexes(update_record->table_name_, curr_record->data,
                                                   update_record->old_value_.data, update_record->rid_);
            } else {
                fh->insert_record(update_record->rid_, update_record->old_value_.data);
                sm_manager_->insert_into_memory_indexes(update_record->table_name_, update_record->old_value_.data,
                                                        update_record->rid_);
            }
        }
    }

    for (auto &fh_pair : sm_manager_->fhs_) {
        buffer_pool_manager_->flush_all_pages(fh_pair.second->GetFd());
    }
}
