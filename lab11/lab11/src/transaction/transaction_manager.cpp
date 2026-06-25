/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <vector>

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {
void release_all_locks(Transaction *txn, LockManager *lock_manager) {
    if (txn == nullptr || lock_manager == nullptr) return;
    std::vector<LockDataId> lock_ids(txn->get_lock_set()->begin(), txn->get_lock_set()->end());
    for (const auto &lock_id : lock_ids) {
        lock_manager->unlock(txn, lock_id);
    }
}

void append_txn_log(Transaction *txn, LogManager *log_manager, LogRecord *record) {
    if (txn == nullptr || log_manager == nullptr || record == nullptr) return;
    record->prev_lsn_ = txn->get_prev_lsn();
    lsn_t lsn = log_manager->add_log_to_buffer(record);
    txn->set_prev_lsn(lsn);
}
}

Transaction *TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        txn = new Transaction(txn_id);
        txn->set_start_ts(next_timestamp_++);
        txn->set_state(TransactionState::GROWING);
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    lock.unlock();
    return txn;
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) return;
    if (log_manager != nullptr && txn->get_prev_lsn() != INVALID_LSN) {
        CommitLogRecord commit_log(txn->get_transaction_id());
        append_txn_log(txn, log_manager, &commit_log);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::COMMITTED);

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }

    release_all_locks(txn, lock_manager_);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) return;

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();
        const std::string tab_name = write_record->GetTableName();
        Rid rid = write_record->GetRid();
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
            auto rec = fh->get_record(rid, nullptr);
            sm_manager_->delete_from_memory_indexes(tab_name, rec->data, &rid);
            fh->delete_record(rid, nullptr);
        } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
            RmRecord &old_rec = write_record->GetRecord();
            fh->insert_record(rid, old_rec.data);
            sm_manager_->insert_into_memory_indexes(tab_name, old_rec.data, rid);
        } else if (write_record->GetWriteType() == WType::UPDATE_TUPLE) {
            auto curr_rec = fh->get_record(rid, nullptr);
            RmRecord &old_rec = write_record->GetRecord();
            fh->update_record(rid, old_rec.data, nullptr);
            sm_manager_->update_memory_indexes(tab_name, curr_rec->data, old_rec.data, rid);
        }
        delete write_record;
    }

    if (log_manager != nullptr && txn->get_prev_lsn() != INVALID_LSN) {
        AbortLogRecord abort_log(txn->get_transaction_id());
        append_txn_log(txn, log_manager, &abort_log);
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::ABORTED);
    release_all_locks(txn, lock_manager_);

    std::unique_lock<std::mutex> lock(latch_);
    txn_map.erase(txn->get_transaction_id());
    lock.unlock();
}
