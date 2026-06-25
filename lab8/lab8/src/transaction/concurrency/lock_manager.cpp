/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::is_compatible(LockMode held_mode, LockMode requested_mode) {
    if (held_mode == LockMode::INTENTION_SHARED) {
        return requested_mode != LockMode::EXLUCSIVE;
    }
    if (held_mode == LockMode::INTENTION_EXCLUSIVE) {
        return requested_mode == LockMode::INTENTION_SHARED ||
               requested_mode == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held_mode == LockMode::SHARED) {
        return requested_mode == LockMode::INTENTION_SHARED ||
               requested_mode == LockMode::SHARED;
    }
    if (held_mode == LockMode::S_IX) {
        return requested_mode == LockMode::INTENTION_SHARED;
    }
    return false;
}

bool LockManager::covers(LockMode held_mode, LockMode requested_mode) {
    if (held_mode == requested_mode) return true;
    if (held_mode == LockMode::EXLUCSIVE) return true;
    if (held_mode == LockMode::S_IX) {
        return requested_mode == LockMode::SHARED ||
               requested_mode == LockMode::INTENTION_SHARED ||
               requested_mode == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held_mode == LockMode::SHARED) {
        return requested_mode == LockMode::INTENTION_SHARED;
    }
    if (held_mode == LockMode::INTENTION_EXCLUSIVE) {
        return requested_mode == LockMode::INTENTION_SHARED;
    }
    return false;
}

LockManager::GroupLockMode LockManager::group_mode(const std::list<LockRequest>& request_queue) {
    bool has_is = false;
    bool has_ix = false;
    bool has_s = false;
    bool has_x = false;
    bool has_six = false;
    for (const auto& request : request_queue) {
        if (!request.granted_) continue;
        has_is = has_is || request.lock_mode_ == LockMode::INTENTION_SHARED;
        has_ix = has_ix || request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE;
        has_s = has_s || request.lock_mode_ == LockMode::SHARED;
        has_x = has_x || request.lock_mode_ == LockMode::EXLUCSIVE;
        has_six = has_six || request.lock_mode_ == LockMode::S_IX;
    }
    if (has_x) return GroupLockMode::X;
    if (has_six || (has_s && has_ix)) return GroupLockMode::SIX;
    if (has_s) return GroupLockMode::S;
    if (has_ix) return GroupLockMode::IX;
    if (has_is) return GroupLockMode::IS;
    return GroupLockMode::NON_LOCK;
}

bool LockManager::lock(Transaction* txn, LockDataId lock_data_id, LockMode lock_mode) {
    if (txn == nullptr) return true;
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::ABORTED) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    std::unique_lock<std::mutex> lock_guard(latch_);
    auto& request_queue = lock_table_[lock_data_id];
    const txn_id_t txn_id = txn->get_transaction_id();

    auto self = request_queue.request_queue_.end();
    for (auto it = request_queue.request_queue_.begin(); it != request_queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn_id) {
            self = it;
            break;
        }
    }

    if (self != request_queue.request_queue_.end() && self->granted_ && covers(self->lock_mode_, lock_mode)) {
        txn->get_lock_set()->insert(lock_data_id);
        return true;
    }

    for (auto it = request_queue.request_queue_.begin(); it != request_queue.request_queue_.end(); ++it) {
        if (!it->granted_ || it->txn_id_ == txn_id) continue;
        if (!is_compatible(it->lock_mode_, lock_mode) || !is_compatible(lock_mode, it->lock_mode_)) {
            throw TransactionAbortException(txn_id, AbortReason::DEADLOCK_PREVENTION);
        }
    }

    if (self == request_queue.request_queue_.end()) {
        request_queue.request_queue_.emplace_back(txn_id, lock_mode);
        self = std::prev(request_queue.request_queue_.end());
    } else {
        self->lock_mode_ = lock_mode;
    }
    self->granted_ = true;
    request_queue.group_lock_mode_ = group_mode(request_queue.request_queue_);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IS_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IX_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;

    std::unique_lock<std::mutex> lock_guard(latch_);
    auto table_it = lock_table_.find(lock_data_id);
    if (table_it == lock_table_.end()) return false;

    auto& request_queue = table_it->second;
    const txn_id_t txn_id = txn->get_transaction_id();
    for (auto it = request_queue.request_queue_.begin(); it != request_queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn_id) {
            request_queue.request_queue_.erase(it);
            txn->get_lock_set()->erase(lock_data_id);
            if (txn->get_state() == TransactionState::GROWING) {
                txn->set_state(TransactionState::SHRINKING);
            }
            if (request_queue.request_queue_.empty()) {
                lock_table_.erase(table_it);
            } else {
                request_queue.group_lock_mode_ = group_mode(request_queue.request_queue_);
                request_queue.cv_.notify_all();
            }
            return true;
        }
    }
    return false;
}
