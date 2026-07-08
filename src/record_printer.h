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

#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include "common/context.h"
#include "common/config.h"

#define RECORD_COUNT_LENGTH 40

class RecordPrinter {
    static constexpr size_t COL_WIDTH = 16;
    size_t num_cols;
    std::vector<size_t> col_widths_;
public:
    RecordPrinter(size_t num_cols_) : num_cols(num_cols_) {
        assert(num_cols_ > 0);
        col_widths_.resize(num_cols_, COL_WIDTH);
    }

    RecordPrinter(size_t num_cols_, std::vector<size_t> col_widths)
        : num_cols(num_cols_), col_widths_(std::move(col_widths)) {
        assert(num_cols_ > 0 && col_widths_.size() == num_cols_);
    }

    void print_separator(Context *context) const {
        for (size_t i = 0; i < num_cols; i++) {
            size_t w = col_widths_[i];
            std::string str = "+" + std::string(w + 2, '-');
            if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
                memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
                *(context->offset_) = *(context->offset_) + str.length();
            }
            else {
                context->ellipsis_ = true;
            }
        }
        std::string str = "+\n";
        if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
            *(context->offset_) = *(context->offset_) + str.length();
        }
        else {
            context->ellipsis_ = true;
        }
    }

    void print_record(const std::vector<std::string> &rec_str, Context *context) const {
        assert(rec_str.size() == num_cols);
        for (size_t i = 0; i < rec_str.size(); i++) {
            std::string col = rec_str[i];
            size_t cw = col_widths_[i];
            if (col.size() > cw) {
                col = col.substr(0, cw - 3) + "...";
            }
            std::stringstream ss;
            ss << "| " << std::setw(cw) << col << " ";
            if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + ss.str().length() < BUFFER_LENGTH) {
                memcpy(context->data_send_ + *(context->offset_), ss.str().c_str(), ss.str().length());
                *(context->offset_) = *(context->offset_) + ss.str().length();
            }
            else {
                context->ellipsis_ = true;
            }
        }
        std::string str = "|\n";
        if(context->ellipsis_ == false && *context->offset_ + RECORD_COUNT_LENGTH + str.length() < BUFFER_LENGTH) {
            memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
            *(context->offset_) = *(context->offset_) + str.length();
        }
    }

    static void print_record_count(size_t num_rec, Context *context) {
        std::string str = "";
        if(context->ellipsis_ == true) {
            str = "... ...\n";
        }
        str += "Total record(s): " + std::to_string(num_rec) + '\n';
        memcpy(context->data_send_ + *(context->offset_), str.c_str(), str.length());
        *(context->offset_) = *(context->offset_) + str.length();
    }
};
