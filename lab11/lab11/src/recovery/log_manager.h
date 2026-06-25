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

#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "log_defs.h"
#include "record/rm_defs.h"

enum LogType : int {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT
};

static std::string LogTypeStr[] = {
    "UPDATE",
    "INSERT",
    "DELETE",
    "BEGIN",
    "COMMIT",
    "ABORT"
};

class LogRecord {
public:
    LogType log_type_;
    lsn_t lsn_;
    uint32_t log_tot_len_;
    txn_id_t log_tid_;
    lsn_t prev_lsn_;

    LogRecord()
        : log_type_(begin),
          lsn_(INVALID_LSN),
          log_tot_len_(LOG_HEADER_SIZE),
          log_tid_(INVALID_TXN_ID),
          prev_lsn_(INVALID_LSN) {}

    virtual ~LogRecord() = default;

    virtual void serialize(char *dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }

    virtual void deserialize(const char *src) {
        log_type_ = *reinterpret_cast<const LogType *>(src + OFFSET_LOG_TYPE);
        lsn_ = *reinterpret_cast<const lsn_t *>(src + OFFSET_LSN);
        log_tot_len_ = *reinterpret_cast<const uint32_t *>(src + OFFSET_LOG_TOT_LEN);
        log_tid_ = *reinterpret_cast<const txn_id_t *>(src + OFFSET_LOG_TID);
        prev_lsn_ = *reinterpret_cast<const lsn_t *>(src + OFFSET_PREV_LSN);
    }

    virtual void format_print() {
        printf("Print Log Record:\n");
        printf("log_type_: %s\n", LogTypeStr[log_type_].c_str());
        printf("lsn: %d\n", lsn_);
        printf("log_tot_len: %u\n", log_tot_len_);
        printf("log_tid: %d\n", log_tid_);
        printf("prev_lsn: %d\n", prev_lsn_);
    }
};

class BeginLogRecord : public LogRecord {
public:
    BeginLogRecord() {
        log_type_ = LogType::begin;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    explicit BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() {
        log_tid_ = txn_id;
    }
};

class CommitLogRecord : public LogRecord {
public:
    CommitLogRecord() {
        log_type_ = LogType::commit;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    explicit CommitLogRecord(txn_id_t txn_id) : CommitLogRecord() {
        log_tid_ = txn_id;
    }
};

class AbortLogRecord : public LogRecord {
public:
    AbortLogRecord() {
        log_type_ = LogType::ABORT;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    explicit AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() {
        log_tid_ = txn_id;
    }
};

class InsertLogRecord : public LogRecord {
public:
    InsertLogRecord() {
        log_type_ = LogType::INSERT;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    InsertLogRecord(txn_id_t txn_id, const RmRecord &insert_value, const Rid &rid, const std::string &table_name)
        : InsertLogRecord() {
        log_tid_ = txn_id;
        insert_value_ = insert_value;
        rid_ = rid;
        table_name_ = table_name;
        log_tot_len_ += sizeof(int) + insert_value_.size + sizeof(Rid) + sizeof(size_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        write_record(dest, offset, insert_value_);
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        write_table_name(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        read_record(src, offset, insert_value_);
        rid_ = *reinterpret_cast<const Rid *>(src + offset);
        offset += sizeof(Rid);
        read_table_name(src, offset, table_name_);
    }

    void format_print() override {
        printf("insert record\n");
        LogRecord::format_print();
        printf("insert rid: %d, %d\n", rid_.page_no, rid_.slot_no);
        printf("table name: %s\n", table_name_.c_str());
    }

    RmRecord insert_value_;
    Rid rid_;
    std::string table_name_;

protected:
    static void write_record(char *dest, int &offset, const RmRecord &record) {
        memcpy(dest + offset, &record.size, sizeof(int));
        offset += sizeof(int);
        memcpy(dest + offset, record.data, record.size);
        offset += record.size;
    }

    static void read_record(const char *src, int &offset, RmRecord &record) {
        record.Deserialize(src + offset);
        offset += sizeof(int) + record.size;
    }

    static void write_table_name(char *dest, int &offset, const std::string &table_name) {
        size_t table_name_size = table_name.size();
        memcpy(dest + offset, &table_name_size, sizeof(size_t));
        offset += sizeof(size_t);
        memcpy(dest + offset, table_name.data(), table_name_size);
        offset += static_cast<int>(table_name_size);
    }

    static void read_table_name(const char *src, int &offset, std::string &table_name) {
        size_t table_name_size = *reinterpret_cast<const size_t *>(src + offset);
        offset += sizeof(size_t);
        table_name.assign(src + offset, table_name_size);
        offset += static_cast<int>(table_name_size);
    }
};

class DeleteLogRecord : public InsertLogRecord {
public:
    DeleteLogRecord() {
        log_type_ = LogType::DELETE;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    DeleteLogRecord(txn_id_t txn_id, const RmRecord &delete_value, const Rid &rid, const std::string &table_name)
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        delete_value_ = delete_value;
        rid_ = rid;
        table_name_ = table_name;
        log_tot_len_ += sizeof(int) + delete_value_.size + sizeof(Rid) + sizeof(size_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        write_record(dest, offset, delete_value_);
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        write_table_name(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        read_record(src, offset, delete_value_);
        rid_ = *reinterpret_cast<const Rid *>(src + offset);
        offset += sizeof(Rid);
        read_table_name(src, offset, table_name_);
    }

    RmRecord delete_value_;
    Rid rid_;
    std::string table_name_;
};

class UpdateLogRecord : public InsertLogRecord {
public:
    UpdateLogRecord() {
        log_type_ = LogType::UPDATE;
        log_tot_len_ = LOG_HEADER_SIZE;
    }

    UpdateLogRecord(txn_id_t txn_id, const RmRecord &old_value, const RmRecord &new_value, const Rid &rid,
                    const std::string &table_name)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        old_value_ = old_value;
        new_value_ = new_value;
        rid_ = rid;
        table_name_ = table_name;
        log_tot_len_ += sizeof(int) + old_value_.size + sizeof(int) + new_value_.size + sizeof(Rid) +
                        sizeof(size_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        write_record(dest, offset, old_value_);
        write_record(dest, offset, new_value_);
        memcpy(dest + offset, &rid_, sizeof(Rid));
        offset += sizeof(Rid);
        write_table_name(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        read_record(src, offset, old_value_);
        read_record(src, offset, new_value_);
        rid_ = *reinterpret_cast<const Rid *>(src + offset);
        offset += sizeof(Rid);
        read_table_name(src, offset, table_name_);
    }

    RmRecord old_value_;
    RmRecord new_value_;
    Rid rid_;
    std::string table_name_;
};

class LogBuffer {
public:
    LogBuffer() {
        offset_ = 0;
        memset(buffer_, 0, sizeof(buffer_));
    }

    bool is_full(int append_size) const {
        return offset_ + append_size > LOG_BUFFER_SIZE;
    }

    char buffer_[LOG_BUFFER_SIZE + 1];
    int offset_;
};

class LogManager {
public:
    explicit LogManager(DiskManager *disk_manager) : persist_lsn_(INVALID_LSN), disk_manager_(disk_manager) {}

    lsn_t add_log_to_buffer(LogRecord *log_record);
    void flush_log_to_disk();

    LogBuffer *get_log_buffer() { return &log_buffer_; }

private:
    void flush_log_to_disk_locked();

    std::atomic<lsn_t> global_lsn_{0};
    std::mutex latch_;
    LogBuffer log_buffer_;
    lsn_t persist_lsn_;
    DiskManager *disk_manager_;
};
