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

class AggregationExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    AggType agg_type_;
    TabCol agg_col_;
    std::string col_name_;
    ColMeta result_col_;
    size_t len_;
    std::vector<ColMeta> cols_;

    bool isend_ = false;
    std::unique_ptr<RmRecord> cached_result_;

    bool is_count_star_ = false;  // COUNT(*)
    int src_offset_ = 0;
    ColType src_type_ = TYPE_INT;

   public:
    AggregationExecutor(std::unique_ptr<AbstractExecutor> prev, AggType agg_type,
                        TabCol agg_col, std::string col_name)
        : prev_(std::move(prev)), agg_type_(agg_type), agg_col_(std::move(agg_col)),
          col_name_(std::move(col_name)) {

        is_count_star_ = (agg_type_ == AGG_COUNT && col_name_.empty());
        len_ = sizeof(int);  // default INT result, can be overridden

        // Determine source column offset/type from prev_'s cols
        if (!is_count_star_) {
            const auto &prev_cols = prev_->cols();
            for (size_t i = 0; i < prev_cols.size(); i++) {
                if (prev_cols[i].tab_name == agg_col_.tab_name
                    && prev_cols[i].name == agg_col_.col_name) {
                    src_offset_ = prev_cols[i].offset;
                    src_type_ = prev_cols[i].type;
                    break;
                }
            }
        }

        // SUM(float) → FLOAT result, same FLOAT type
        if (agg_type_ == AGG_SUM && src_type_ == TYPE_FLOAT) {
            len_ = sizeof(float);
            result_col_ = {.tab_name = "", .name = col_name_,
                           .type = TYPE_FLOAT, .len = sizeof(float), .offset = 0, .index = false};
        } else if (agg_type_ == AGG_MAX || agg_type_ == AGG_MIN) {
            len_ = (src_type_ == TYPE_FLOAT) ? sizeof(float) : sizeof(int);
            ColType etype = (src_type_ == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            result_col_ = {.tab_name = "", .name = col_name_,
                           .type = etype, .len = (int)len_, .offset = 0, .index = false};
        } else {
            // COUNT, SUM(INT) → INT result
            result_col_ = {.tab_name = "", .name = col_name_,
                           .type = TYPE_INT, .len = sizeof(int), .offset = 0, .index = false};
        }
        cols_.push_back(result_col_);
    }

    void beginTuple() override {
        if (isend_) return;
        prev_->beginTuple();

        if (agg_type_ == AGG_COUNT) {
            int count = 0;
            for (; !prev_->is_end(); prev_->nextTuple()) {
                if (is_count_star_) {
                    count++;
                } else {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    count++;
                }
            }
            cached_result_ = std::make_unique<RmRecord>(sizeof(int));
            *(int*)cached_result_->data = count;
        } else if (agg_type_ == AGG_SUM) {
            if (src_type_ == TYPE_FLOAT) {
                float sum = 0.0f;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    sum += *(float*)(rec->data + src_offset_);
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(float));
                *(float*)cached_result_->data = sum;
            } else {
                int sum = 0;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    sum += *(int*)(rec->data + src_offset_);
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(int));
                *(int*)cached_result_->data = sum;
            }
        } else if (agg_type_ == AGG_MAX) {
            if (src_type_ == TYPE_FLOAT) {
                float max_val = -1e30f;
                bool first = true;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    float v = *(float*)(rec->data + src_offset_);
                    if (first || v > max_val) { max_val = v; first = false; }
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(float));
                *(float*)cached_result_->data = max_val;
            } else {
                int max_val = INT32_MIN;
                bool first = true;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    int v = *(int*)(rec->data + src_offset_);
                    if (first || v > max_val) { max_val = v; first = false; }
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(int));
                *(int*)cached_result_->data = max_val;
            }
        } else if (agg_type_ == AGG_MIN) {
            if (src_type_ == TYPE_FLOAT) {
                float min_val = 1e30f;
                bool first = true;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    float v = *(float*)(rec->data + src_offset_);
                    if (first || v < min_val) { min_val = v; first = false; }
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(float));
                *(float*)cached_result_->data = min_val;
            } else {
                int min_val = INT32_MAX;
                bool first = true;
                for (; !prev_->is_end(); prev_->nextTuple()) {
                    auto rec = prev_->Next();
                    if (rec == nullptr) break;
                    int v = *(int*)(rec->data + src_offset_);
                    if (first || v < min_val) { min_val = v; first = false; }
                }
                cached_result_ = std::make_unique<RmRecord>(sizeof(int));
                *(int*)cached_result_->data = min_val;
            }
        }
    }

    void nextTuple() override { isend_ = true; }

    bool is_end() const override { return isend_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_) return nullptr;
        isend_ = true;
        return std::move(cached_result_);
    }

    Rid &rid() override { return _abstract_rid; }
};
