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
#include <fstream>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    std::unique_ptr<RmRecord> cached_left_;     // 当前左元组缓存

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        if (!left_->is_end()) {
            cached_left_ = left_->Next();
        }
        isend = left_->is_end() || cached_left_ == nullptr;
    }

    void nextTuple() override {
        right_->nextTuple();
        if (!right_->is_end()) {
            return;
        }
        left_->nextTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        cached_left_ = left_->Next();
        right_->beginTuple();
    }

    bool is_end() const override {
        return isend;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override {
        while (!isend) {
            auto right_rec = right_->Next();
            if (right_rec == nullptr) {
                nextTuple();
                if (isend) return nullptr;
                continue;
            }
            auto join_rec = std::make_unique<RmRecord>(len_);
            memcpy(join_rec->data, cached_left_->data, left_->tupleLen());
            memcpy(join_rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
            if (eval_conds(join_rec->data)) {
                return join_rec;
            }
            nextTuple();
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    bool eval_cond(const char* rec_buf, const Condition &cond) {
        // 找到左值列
        const ColMeta* lhs_col = nullptr;
        for (auto &col : cols_) {
            if (col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name) {
                lhs_col = &col;
                break;
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
                    rhs_col = &col;
                    break;
                }
            }
            if (!rhs_col) return true;
            rhs_data = rec_buf + rhs_col->offset;
        }

        int cmp = 0;
        switch (lhs_col->type) {
            case TYPE_INT:
                cmp = (*(int*)lhs_data < *(int*)rhs_data) ? -1 : (*(int*)lhs_data > *(int*)rhs_data) ? 1 : 0;
                break;
            case TYPE_BIGINT:
            case TYPE_DATETIME:
                cmp = (*(int64_t*)lhs_data < *(int64_t*)rhs_data) ? -1 : (*(int64_t*)lhs_data > *(int64_t*)rhs_data) ? 1 : 0;
                break;
            case TYPE_FLOAT:
                cmp = (*(float*)lhs_data < *(float*)rhs_data) ? -1 : ((*(float*)lhs_data > *(float*)rhs_data) ? 1 : 0);
                break;
            case TYPE_STRING:
                cmp = memcmp(lhs_data, rhs_data, lhs_col->len);
                break;
            default:
                throw InternalError("Unexpected data type");
        }

        switch (cond.op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return true;
        }
    }

    bool eval_conds(const char* rec_buf) {
        for (auto &cond : fed_conds_) {
            if (!eval_cond(rec_buf, cond)) {
                return false;
            }
        }
        return true;
    }
};