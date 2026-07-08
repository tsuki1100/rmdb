/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
... */

#include "log_recovery.h"
#include "system/sm.h"
#include <set>
#include <fcntl.h>
#include <unistd.h>

static char* read_log_file(int &total_bytes) {
    int fd = open("db.log", O_RDONLY);
    if (fd < 0) { total_bytes = 0; return nullptr; }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    total_bytes = (int)size;
    if (size == 0) { close(fd); return nullptr; }
    char* buf = new char[size];
    ssize_t r = read(fd, buf, size); (void)r;
    close(fd);
    return buf;
}

static LogRecord* parse_one(const char* data, int &off) {
    LogType t = *reinterpret_cast<const LogType*>(data + off);
    uint32_t tot_len = *reinterpret_cast<const uint32_t*>(data + off + OFFSET_LOG_TOT_LEN);
    LogRecord* rec = nullptr;
    switch (t) {
        case LogType::begin: rec = new BeginLogRecord(); break;
        case LogType::commit: rec = new CommitLogRecord(); break;
        case LogType::ABORT: rec = new AbortLogRecord(); break;
        case LogType::INSERT: rec = new InsertLogRecord(); break;
        case LogType::DELETE: rec = new DeleteLogRecord(); break;
        case LogType::UPDATE: rec = new UpdateLogRecord(); break;
        default: off += tot_len; return nullptr;
    }
    rec->deserialize(data + off);
    off += tot_len;
    return rec;
}

void RecoveryManager::analyze() {
    int total = 0;
    char* log_data = read_log_file(total);
    if (!log_data) return;
    int off = 0;
    while (off < total) {
        LogRecord* rec = parse_one(log_data, off);
        if (!rec) continue;
        if (rec->log_type_ == LogType::begin) {
            att_[rec->log_tid_] = TransactionState::DEFAULT;
        } else if (rec->log_type_ == LogType::commit) {
            att_[rec->log_tid_] = TransactionState::COMMITTED;
        } else if (rec->log_type_ == LogType::ABORT) {
            att_.erase(rec->log_tid_);
        } else {
            all_logs_.push_back(rec);
            continue;
        }
        delete rec;
    }
    delete[] log_data;
}

void RecoveryManager::redo() {
    for (auto* rec : all_logs_) {
        if (att_.count(rec->log_tid_) == 0 || att_[rec->log_tid_] == TransactionState::ABORTED)
            continue;
        apply_redo(rec);
    }
}

void RecoveryManager::undo() {
    std::set<txn_id_t> to_undo;
    for (auto &p : att_) {
        if (p.second == TransactionState::DEFAULT) to_undo.insert(p.first);
    }
    for (auto it = all_logs_.rbegin(); it != all_logs_.rend(); ++it) {
        auto* rec = *it;
        if (to_undo.count(rec->log_tid_)) apply_undo(rec);
    }
    for (auto* rec : all_logs_) delete rec;
    all_logs_.clear();
}

void RecoveryManager::apply_redo(LogRecord* rec) {
    auto &fhs = sm_manager_->fhs_;
    if (rec->log_type_ == LogType::INSERT) {
        auto* ir = dynamic_cast<InsertLogRecord*>(rec);
        if (!fhs.count(ir->table_name_)) return;
        fhs[ir->table_name_]->insert_record(ir->insert_value_.data, nullptr);
    } else if (rec->log_type_ == LogType::DELETE) {
        auto* dr = dynamic_cast<DeleteLogRecord*>(rec);
        if (!fhs.count(dr->table_name_)) return;
        fhs[dr->table_name_]->delete_record(dr->rid_, nullptr);
    } else if (rec->log_type_ == LogType::UPDATE) {
        auto* ur = dynamic_cast<UpdateLogRecord*>(rec);
        if (!fhs.count(ur->table_name_)) return;
        fhs[ur->table_name_]->update_record(ur->rid_, ur->new_value_.data, nullptr);
    }
}

void RecoveryManager::apply_undo(LogRecord* rec) {
    auto &fhs = sm_manager_->fhs_;
    if (rec->log_type_ == LogType::INSERT) {
        auto* ir = dynamic_cast<InsertLogRecord*>(rec);
        if (!fhs.count(ir->table_name_)) return;
        fhs[ir->table_name_]->delete_record(ir->rid_, nullptr);
    } else if (rec->log_type_ == LogType::DELETE) {
        auto* dr = dynamic_cast<DeleteLogRecord*>(rec);
        if (!fhs.count(dr->table_name_)) return;
        fhs[dr->table_name_]->insert_record(dr->del_value_.data, nullptr);
    } else if (rec->log_type_ == LogType::UPDATE) {
        auto* ur = dynamic_cast<UpdateLogRecord*>(rec);
        if (!fhs.count(ur->table_name_)) return;
        fhs[ur->table_name_]->update_record(ur->rid_, ur->old_value_.data, nullptr);
    }
}
