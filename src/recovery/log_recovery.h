/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. ...
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS... */

#pragma once
#include <map>
#include <unordered_map>
#include <vector>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"
#include "transaction/txn_defs.h"

class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager) {
        disk_manager_ = disk_manager; buffer_pool_manager_ = buffer_pool_manager; sm_manager_ = sm_manager;
    }
    void analyze();
    void redo();
    void undo();
private:
    void apply_redo(LogRecord* rec);
    void apply_undo(LogRecord* rec);

    DiskManager* disk_manager_;
    BufferPoolManager* buffer_pool_manager_;
    SmManager* sm_manager_;
    std::unordered_map<txn_id_t, TransactionState> att_;  // active txn table
    std::vector<LogRecord*> all_logs_;                      // all data logs in order
};
