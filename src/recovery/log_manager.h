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
#include <mutex>
#include <vector>
#include <iostream>
#include <string>
#include "log_defs.h"
#include "common/config.h"
#include "record/rm_defs.h"

enum LogType: int {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT
};

class LogRecord {
public:
    LogType log_type_;
    lsn_t lsn_;
    uint32_t log_tot_len_;
    txn_id_t log_tid_;
    lsn_t prev_lsn_;

    virtual void serialize (char* dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }
    virtual void deserialize(const char* src) {
        log_type_ = *reinterpret_cast<const LogType*>(src);
        lsn_ = *reinterpret_cast<const lsn_t*>(src + OFFSET_LSN);
        log_tot_len_ = *reinterpret_cast<const uint32_t*>(src + OFFSET_LOG_TOT_LEN);
        log_tid_ = *reinterpret_cast<const txn_id_t*>(src + OFFSET_LOG_TID);
        prev_lsn_ = *reinterpret_cast<const lsn_t*>(src + OFFSET_PREV_LSN);
    }
    virtual ~LogRecord() = default;
    virtual void format_print() {}
};

class BeginLogRecord: public LogRecord {
public:
    BeginLogRecord() { log_type_ = LogType::begin; log_tot_len_ = LOG_HEADER_SIZE; }
    BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() { log_tid_ = txn_id; }
};

class CommitLogRecord: public LogRecord {
public:
    CommitLogRecord() { log_type_ = LogType::commit; log_tot_len_ = LOG_HEADER_SIZE; }
    CommitLogRecord(txn_id_t txn_id) : CommitLogRecord() { log_tid_ = txn_id; }
};

class AbortLogRecord: public LogRecord {
public:
    AbortLogRecord() { log_type_ = LogType::ABORT; log_tot_len_ = LOG_HEADER_SIZE; }
    AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() { log_tid_ = txn_id; }
};

class InsertLogRecord: public LogRecord {
public:
    InsertLogRecord() { log_type_ = LogType::INSERT; log_tot_len_ = LOG_HEADER_SIZE; }
    InsertLogRecord(txn_id_t txn_id, RmRecord& insert_value, Rid& rid, std::string table_name)
        : InsertLogRecord() {
        log_tid_ = txn_id; insert_value_ = insert_value; rid_ = rid;
        table_name_size_ = table_name.length(); table_name_ = table_name;
        log_tot_len_ += sizeof(int) + insert_value_.size + sizeof(Rid) + sizeof(size_t) + table_name_size_;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int off = OFFSET_LOG_DATA;
        memcpy(dest + off, &insert_value_.size, sizeof(int)); off += sizeof(int);
        memcpy(dest + off, insert_value_.data, insert_value_.size); off += insert_value_.size;
        memcpy(dest + off, &rid_, sizeof(Rid)); off += sizeof(Rid);
        memcpy(dest + off, &table_name_size_, sizeof(size_t)); off += sizeof(size_t);
        memcpy(dest + off, table_name_.c_str(), table_name_size_);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        insert_value_.Deserialize(src + OFFSET_LOG_DATA);
        int off = OFFSET_LOG_DATA + insert_value_.size + sizeof(int);
        rid_ = *reinterpret_cast<const Rid*>(src + off); off += sizeof(Rid);
        table_name_size_ = *reinterpret_cast<const size_t*>(src + off); off += sizeof(size_t);
        char* buf = new char[table_name_size_ + 1];
        memcpy(buf, src + off, table_name_size_); buf[table_name_size_] = 0;
        table_name_ = std::string(buf); delete[] buf;
    }
    RmRecord insert_value_; Rid rid_; std::string table_name_; size_t table_name_size_ = 0;
};

class DeleteLogRecord: public LogRecord {
public:
    DeleteLogRecord() { log_type_ = LogType::DELETE; log_tot_len_ = LOG_HEADER_SIZE; }
    DeleteLogRecord(txn_id_t txn_id, RmRecord& del_value, Rid& rid, std::string table_name)
        : DeleteLogRecord() {
        log_tid_ = txn_id; del_value_ = del_value; rid_ = rid;
        table_name_size_ = table_name.length(); table_name_ = table_name;
        log_tot_len_ += sizeof(int) + del_value_.size + sizeof(Rid) + sizeof(size_t) + table_name_size_;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int off = OFFSET_LOG_DATA;
        memcpy(dest + off, &del_value_.size, sizeof(int)); off += sizeof(int);
        memcpy(dest + off, del_value_.data, del_value_.size); off += del_value_.size;
        memcpy(dest + off, &rid_, sizeof(Rid)); off += sizeof(Rid);
        memcpy(dest + off, &table_name_size_, sizeof(size_t)); off += sizeof(size_t);
        memcpy(dest + off, table_name_.c_str(), table_name_size_);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        del_value_.Deserialize(src + OFFSET_LOG_DATA);
        int off = OFFSET_LOG_DATA + del_value_.size + sizeof(int);
        rid_ = *reinterpret_cast<const Rid*>(src + off); off += sizeof(Rid);
        table_name_size_ = *reinterpret_cast<const size_t*>(src + off); off += sizeof(size_t);
        char* buf = new char[table_name_size_ + 1];
        memcpy(buf, src + off, table_name_size_); buf[table_name_size_] = 0;
        table_name_ = std::string(buf); delete[] buf;
    }
    RmRecord del_value_; Rid rid_; std::string table_name_; size_t table_name_size_ = 0;
};

class UpdateLogRecord: public LogRecord {
public:
    UpdateLogRecord() { log_type_ = LogType::UPDATE; log_tot_len_ = LOG_HEADER_SIZE; }
    UpdateLogRecord(txn_id_t txn_id, RmRecord& old_val, RmRecord& new_val, Rid& rid, std::string table_name)
        : UpdateLogRecord() {
        log_tid_ = txn_id; old_value_ = old_val; new_value_ = new_val; rid_ = rid;
        table_name_size_ = table_name.length(); table_name_ = table_name;
        log_tot_len_ += sizeof(int) + old_val.size + sizeof(int) + new_val.size + sizeof(Rid) + sizeof(size_t) + table_name_size_;
    }
    void serialize(char* dest) const override {
        LogRecord::serialize(dest);
        int off = OFFSET_LOG_DATA;
        memcpy(dest + off, &old_value_.size, sizeof(int)); off += sizeof(int);
        memcpy(dest + off, old_value_.data, old_value_.size); off += old_value_.size;
        memcpy(dest + off, &new_value_.size, sizeof(int)); off += sizeof(int);
        memcpy(dest + off, new_value_.data, new_value_.size); off += new_value_.size;
        memcpy(dest + off, &rid_, sizeof(Rid)); off += sizeof(Rid);
        memcpy(dest + off, &table_name_size_, sizeof(size_t)); off += sizeof(size_t);
        memcpy(dest + off, table_name_.c_str(), table_name_size_);
    }
    void deserialize(const char* src) override {
        LogRecord::deserialize(src);
        old_value_.Deserialize(src + OFFSET_LOG_DATA);
        int off = OFFSET_LOG_DATA + old_value_.size + sizeof(int);
        new_value_.Deserialize(src + off); off += new_value_.size + sizeof(int);
        rid_ = *reinterpret_cast<const Rid*>(src + off); off += sizeof(Rid);
        table_name_size_ = *reinterpret_cast<const size_t*>(src + off); off += sizeof(size_t);
        char* buf = new char[table_name_size_ + 1];
        memcpy(buf, src + off, table_name_size_); buf[table_name_size_] = 0;
        table_name_ = std::string(buf); delete[] buf;
    }
    RmRecord old_value_; RmRecord new_value_; Rid rid_; std::string table_name_; size_t table_name_size_ = 0;
};

class LogBuffer {
public:
    LogBuffer() { offset_ = 0; memset(buffer_, 0, sizeof(buffer_)); }
    bool is_full(int append_size) { return offset_ + append_size > LOG_BUFFER_SIZE; }
    char buffer_[LOG_BUFFER_SIZE+1];
    int offset_;
};

class LogManager {
public:
    LogManager(DiskManager* disk_manager) { disk_manager_ = disk_manager; }

    lsn_t add_log_to_buffer(LogRecord* log_record);
    void flush_log_to_disk();

    LogBuffer* get_log_buffer() { return &log_buffer_; }

private:
    std::atomic<lsn_t> global_lsn_{0};
    std::mutex latch_;
    LogBuffer log_buffer_;
    lsn_t persist_lsn_ = 0;
    int log_fd_ = -1;
    DiskManager* disk_manager_;
};
