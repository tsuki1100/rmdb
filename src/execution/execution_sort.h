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
#include <algorithm>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
   public:
    size_t limit_ = SIZE_MAX;

   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<TabCol> sel_cols_;
    std::vector<bool> is_descs_;
    std::vector<ColMeta> cols_;
    size_t len_;

    std::vector<std::unique_ptr<RmRecord>> sorted_;
    size_t cursor_ = 0;    // next tuple to output
    std::unique_ptr<RmRecord> current_;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_descs)
        : prev_(std::move(prev)), sel_cols_(std::move(sel_cols)), is_descs_(std::move(is_descs)) {
        cols_ = prev_->cols();
        len_ = prev_->tupleLen();
    }

    void beginTuple() override {
        // Collect all tuples
        sorted_.clear();
        prev_->beginTuple();
        for (; !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec == nullptr) break;
            sorted_.push_back(std::move(rec));
        }

        // Sort
        std::stable_sort(sorted_.begin(), sorted_.end(),
            [this](const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
                for (size_t k = 0; k < sel_cols_.size(); k++) {
                    const auto &sc = sel_cols_[k];
                    bool desc = is_descs_[k];
                    int offset = -1; ColType type = TYPE_INT; int clen = 0;
                    for (auto &c : cols_) {
                        if (c.name == sc.col_name) { offset = c.offset; type = c.type; clen = c.len; break; }
                    }
                    if (offset < 0) return false;
                    const char *da = a->data + offset, *db = b->data + offset;
                    int cmp = 0;
                    if (type == TYPE_INT) cmp = (*(int*)da < *(int*)db) ? -1 : (*(int*)da > *(int*)db) ? 1 : 0;
                    else if (type == TYPE_BIGINT || type == TYPE_DATETIME) cmp = (*(int64_t*)da < *(int64_t*)db) ? -1 : (*(int64_t*)da > *(int64_t*)db) ? 1 : 0;
                    else if (type == TYPE_FLOAT) cmp = (*(float*)da < *(float*)db) ? -1 : (*(float*)da > *(float*)db) ? 1 : 0;
                    else if (type == TYPE_STRING) cmp = memcmp(da, db, clen);
                    if (cmp != 0) return desc ? (cmp > 0) : (cmp < 0);
                }
                return false;
            });

        cursor_ = 0;
        // Pre-populate current_ with the first tuple (if available)
        current_ = nullptr;
        if (cursor_ < sorted_.size() && cursor_ < limit_) {
            current_ = std::make_unique<RmRecord>(len_);
            memcpy(current_->data, sorted_[cursor_]->data, len_);
            cursor_++;
        }
    }

    void nextTuple() override {
        current_ = nullptr;
        if (cursor_ < sorted_.size() && cursor_ < limit_) {
            current_ = std::make_unique<RmRecord>(len_);
            memcpy(current_->data, sorted_[cursor_]->data, len_);
            cursor_++;
        }
    }

    bool is_end() const override { return current_ == nullptr; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override { return std::move(current_); }

    Rid &rid() override { return _abstract_rid; }
};
