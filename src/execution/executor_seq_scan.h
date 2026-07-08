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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        // IS lock skipped for basic query compatibility
        scan_ = std::make_unique<RmScan>(fh_);
        // 跳过不满足条件的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec->data)) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec->data)) {
                return;
            }
            scan_->next();
        }
    }

    bool is_end() const override {
        return scan_ ? scan_->is_end() : true;
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }

   private:
    bool eval_cond(const char* rec_buf, const Condition &cond) {
        // 找到左值列的元数据
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

        // 比较
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