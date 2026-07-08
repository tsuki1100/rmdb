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

using LM = LockManager;
using G = LM::GroupLockMode;

static bool lock_compatible(G existing, G requested) {
    int c[6][6] = {{1,1,1,1,1,1},{1,1,1,1,1,0},{1,1,1,0,0,0},
                   {1,1,0,1,0,0},{1,1,0,0,0,0},{1,0,0,0,0,0}};
    return c[(int)existing][(int)requested] == 1;
}

static G lock_to_group(LM::LockMode m) {
    switch (m) {
        case LM::LockMode::SHARED: return G::S;
        case LM::LockMode::EXLUCSIVE: return G::X;
        case LM::LockMode::INTENTION_SHARED: return G::IS;
        case LM::LockMode::INTENTION_EXCLUSIVE: return G::IX;
        case LM::LockMode::S_IX: return G::SIX;
        default: return G::NON_LOCK;
    }
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
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

bool LockManager::lock(Transaction* txn, LockDataId lock_data_id, LockMode lock_mode) {
    std::unique_lock<std::mutex> lk(latch_);
    auto &queue = lock_table_[lock_data_id];
    G req_g = lock_to_group(lock_mode);

    for (auto &r : queue.request_queue_) {
        if (!r.granted_) continue;
        if (r.txn_id_ == txn->get_transaction_id()) {
            // Upgrade S -> X
            if (lock_mode == LockMode::EXLUCSIVE && r.lock_mode_ == LockMode::SHARED) {
                bool solo = true;
                for (auto &r2 : queue.request_queue_)
                    if (r2.granted_ && r2.txn_id_ != txn->get_transaction_id()) { solo = false; break; }
                if (solo) { r.lock_mode_ = LockMode::EXLUCSIVE; queue.group_lock_mode_ = req_g; return true; }
            }
            if (r.lock_mode_ == LockMode::EXLUCSIVE) return true;
            if (r.lock_mode_ == LockMode::SHARED && lock_mode == LockMode::SHARED) return true;
            continue;
        }
        if (!lock_compatible(lock_to_group(r.lock_mode_), req_g))
            return false;  // no-wait
    }

    LockRequest req(txn->get_transaction_id(), lock_mode);
    req.granted_ = true;
    queue.request_queue_.push_back(req);
    queue.group_lock_mode_ = req_g;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::unique_lock<std::mutex> lk(latch_);
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return true;
    auto &queue = it->second;
    queue.request_queue_.remove_if(
        [txn](const LockRequest &r) { return r.txn_id_ == txn->get_transaction_id(); });
    G g = G::NON_LOCK;
    for (auto &r : queue.request_queue_) {
        if (!r.granted_) continue;
        if ((int)lock_to_group(r.lock_mode_) > (int)g) g = lock_to_group(r.lock_mode_);
    }
    queue.group_lock_mode_ = g;
    if (queue.request_queue_.empty()) lock_table_.erase(it);
    return true;
}
