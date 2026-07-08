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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        auto ih = sm_manager_->ihs_.at(
            sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        // 构建索引查找键值
        char* key = new char[index_meta_.col_tot_len];
        memset(key, 0, index_meta_.col_tot_len);
        bool has_eq = false;
        for (auto &cond : conds_) {
            if (cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name == tab_name_) {
                for (size_t i = 0; i < index_meta_.col_num; i++) {
                    if (index_meta_.cols[i].name == cond.lhs_col.col_name) {
                        int offset = 0;
                        for (size_t j = 0; j < i; j++) {
                            offset += index_meta_.cols[j].len;
                        }
                        memcpy(key + offset, cond.rhs_val.raw->data, index_meta_.cols[i].len);
                        has_eq = true;
                        break;
                    }
                }
            }
        }
        // 获取索引句柄并进行范围扫描
        Iid lower, upper;
        if (has_eq) {
            lower = ih->lower_bound(key);
            upper = ih->upper_bound(key);
        } else {
            // Range-only query: scan entire index from first to last leaf
            lower = ih->leaf_begin();
            upper = ih->leaf_end();
        }
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        delete[] key;
        // 定位到第一条满足非索引条件的记录
        skip_invalid();
    }

    void nextTuple() override {
        scan_->next();
        skip_invalid();
    }

    bool is_end() const override {
        return scan_ ? scan_->is_end() : true;
    }

    std::unique_ptr<RmRecord> Next() override {
        rid_ = scan_->rid();
        return fh_->get_record(rid_, context_);
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    Rid &rid() override { return rid_; }

   private:
    void skip_invalid() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec->data)) {
                return;
            }
            scan_->next();
        }
    }

    bool eval_cond(const char* rec_buf, const Condition &cond) {
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