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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class BlockNestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t left_len_, right_len_, len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;
    bool isend_ = true;

    // Join buffer
    static constexpr size_t JOIN_BUFFER_SIZE = 65536;
    std::vector<std::unique_ptr<RmRecord>> left_block_;
    size_t block_cursor_ = 0;
    bool need_load_block_ = true;
    bool left_exhausted_ = false;
    std::unique_ptr<RmRecord> cached_right_;

   public:
    BlockNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                                std::unique_ptr<AbstractExecutor> right,
                                std::vector<Condition> conds)
        : left_(std::move(left)), right_(std::move(right)) {
        left_len_ = left_->tupleLen();
        right_len_ = right_->tupleLen();
        len_ = left_len_ + right_len_;
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) col.offset += left_len_;
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_exhausted_ = false;
        need_load_block_ = true;
        left_block_.clear();
        block_cursor_ = 0;
        isend_ = false;
        left_->beginTuple();

        if (!load_block()) { isend_ = true; return; }
        if (!open_right_scan()) { isend_ = true; return; }

        // Try to find first match
        bool found = search_matches();
        if (!found) {
            // Advance right and re-search
            while (advance_right()) {
                if (search_matches()) return;
            }
            isend_ = true;
        }
    }

    void nextTuple() override {
        if (isend_) return;
        // Skip current match, try next left in block
        block_cursor_++;
        if (search_matches()) return;
        // Try next right tuple
        while (advance_right()) {
            if (search_matches()) return;
        }
        isend_ = true;
    }

    bool is_end() const override { return isend_; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_) return nullptr;
        size_t idx = block_cursor_;
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_block_[idx]->data, left_len_);
        memcpy(rec->data + left_len_, cached_right_->data, right_len_);
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    bool load_block() {
        left_block_.clear();
        block_cursor_ = 0;
        size_t block_bytes = 0;
        while (!left_->is_end()) {
            auto rec = left_->Next();
            if (rec == nullptr) { left_->nextTuple(); continue; }
            left_block_.push_back(std::move(rec));
            block_bytes += left_len_;
            left_->nextTuple();
            if (block_bytes + left_len_ > JOIN_BUFFER_SIZE) break;
        }
        need_load_block_ = false;
        if (left_block_.empty()) {
            left_exhausted_ = true;
            return false;
        }
        return true;
    }

    bool open_right_scan() {
        right_->beginTuple();
        if (right_->is_end()) return false;
        cached_right_ = right_->Next();
        return cached_right_ != nullptr;
    }

    bool search_matches() {
        // Search left_block from block_cursor_ to end
        std::vector<char> tmp(len_);
        for (; block_cursor_ < left_block_.size(); block_cursor_++) {
            memcpy(tmp.data(), left_block_[block_cursor_]->data, left_len_);
            memcpy(tmp.data() + left_len_, cached_right_->data, right_len_);
            if (eval_conds(tmp.data())) return true;
        }
        return false;  // no match found in remaining block
    }

    bool advance_right() {
        right_->nextTuple();
        if (right_->is_end()) {
            // Load next block of left tuples, restart right scan
            if (!load_block()) return false;
            if (!open_right_scan()) return false;
        } else {
            cached_right_ = right_->Next();
            if (cached_right_ == nullptr) {
                right_->nextTuple();
                if (right_->is_end()) {
                    if (!load_block()) return false;
                    if (!open_right_scan()) return false;
                } else {
                    cached_right_ = right_->Next();
                    if (cached_right_ == nullptr) return false;
                }
            }
        }
        block_cursor_ = 0;  // Reset for new right tuple
        return true;
    }

    bool eval_cond(const char* rec_buf, const Condition &cond) {
        const ColMeta* lhs_col = nullptr;
        for (auto &col : cols_) {
            if (col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name) {
                lhs_col = &col; break;
            }
        }
        if (!lhs_col) return true;
        const char* lhs_data = rec_buf + lhs_col->offset;
        const char* rhs_data;
        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
        } else {
            const ColMeta* rhs_col = nullptr;
            for (auto &col : cols_) {
                if (col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name) {
                    rhs_col = &col; break;
                }
            }
            if (!rhs_col) return true;
            rhs_data = rec_buf + rhs_col->offset;
        }
        int cmp = 0;
        switch (lhs_col->type) {
            case TYPE_INT:
                cmp = (*(int*)lhs_data < *(int*)rhs_data) ? -1 : (*(int*)lhs_data > *(int*)rhs_data) ? 1 : 0; break;
            case TYPE_BIGINT: case TYPE_DATETIME:
                cmp = (*(int64_t*)lhs_data < *(int64_t*)rhs_data) ? -1 : (*(int64_t*)lhs_data > *(int64_t*)rhs_data) ? 1 : 0; break;
            case TYPE_FLOAT:
                cmp = (*(float*)lhs_data < *(float*)rhs_data) ? -1 : (*(float*)lhs_data > *(float*)rhs_data) ? 1 : 0; break;
            case TYPE_STRING:
                cmp = memcmp(lhs_data, rhs_data, lhs_col->len); break;
            default: return true;
        }
        switch (cond.op) {
            case OP_EQ: return cmp == 0; case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;  case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0; case OP_GE: return cmp >= 0;
            default: return true;
        }
    }

    bool eval_conds(const char* rec_buf) {
        for (auto &cond : fed_conds_) if (!eval_cond(rec_buf, cond)) return false;
        return true;
    }
};
