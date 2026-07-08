/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <experimental/filesystem>
#include <fstream>
#include <iostream>

#include "index/ix_manager.h"
#include "record/rm_manager.h"
#include "record/rm_scan.h"
#include "record_printer.h"

using namespace std;

bool SmManager::is_dir(const string &db_name) {
    std::error_code ec;
    return std::experimental::filesystem::is_directory(db_name, ec);
}

void SmManager::create_db(const string &db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;
    ofstream ofs(DB_META_NAME);
    ofs << *new_db;
    delete new_db;
    disk_manager_->create_file(LOG_FILE_NAME);
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

void SmManager::drop_db(const string &db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

void SmManager::open_db(const string &db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }
    ifstream ifs(DB_META_NAME);
    if (!ifs.is_open()) {
        throw DatabaseNotFoundError(db_name);
    }
    ifs >> db_;
    for (auto &entry : db_.tabs_) {
        auto &tab_name = entry.first;
        auto &tab = entry.second;
        fhs_[tab_name] = rm_manager_->open_file(tab_name);
        for (auto &index : tab.indexes) {
            std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
            ihs_[ix_name] = ix_manager_->open_index(tab_name, index.cols);
        }
    }
}

void SmManager::close_db() {
    flush_meta();
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    fhs_.clear();
    ihs_.clear();
}

void SmManager::flush_meta() {
    ofstream ofs(DB_META_NAME);
    ofs << db_;
}

void SmManager::show_tables(Context *context) {
    RecordPrinter rec_printer(1);
    rec_printer.print_separator(context);
    rec_printer.print_record({"Tables"}, context);
    rec_printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        rec_printer.print_record({entry.first}, context);
    }
    rec_printer.print_separator(context);
}

void SmManager::desc_table(const string &tab_name, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    RecordPrinter rec_printer(3);
    rec_printer.print_separator(context);
    rec_printer.print_record({"Field", "Type", "Index"}, context);
    rec_printer.print_separator(context);
    for (auto &col : tab.cols) {
        vector<string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        rec_printer.print_record(field_info, context);
    }
    rec_printer.print_separator(context);
}

void SmManager::create_table(const string &tab_name, const vector<ColDef> &col_defs, Context *context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    int record_size = 0;
    for (auto &col_def : col_defs) {
        record_size += col_def.len;
    }
    rm_manager_->create_file(tab_name, record_size);
    fhs_[tab_name] = rm_manager_->open_file(tab_name);
    TabMeta tab;
    tab.name = tab_name;
    int offset = 0;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name, .name = col_def.name, .type = col_def.type, .len = col_def.len, .offset = offset, .index = false};
        tab.cols.push_back(col);
        offset += col_def.len;
    }
    db_.SetTabMeta(tab_name, tab);
    flush_meta();
}

void SmManager::drop_table(const string &tab_name, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    for (auto &index : tab.indexes) {
        std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
        if (ihs_.count(ix_name)) {
            ix_manager_->close_index(ihs_[ix_name].get());
            ihs_.erase(ix_name);
        }
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    rm_manager_->close_file(fhs_[tab_name].get());
    fhs_.erase(tab_name);
    rm_manager_->destroy_file(tab_name);
    string cmd = "rm " + tab_name + ".txt";
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
    db_.tabs_.erase(tab_name);
    flush_meta();
}

void SmManager::create_index(const string &tab_name, const vector<string> &col_names, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }
    vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
    }
    ix_manager_->create_index(tab_name, index_cols);
    std::string ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    ihs_[ix_name] = ix_manager_->open_index(tab_name, index_cols);
    auto ih = ihs_[ix_name].get();
    // Build existing entries
    RmFileHandle *fh = fhs_[tab_name].get();
    RmScan scan(fh);
    int col_tot_len = 0;
    for (auto &col : index_cols) col_tot_len += col.len;
    while (!scan.is_end()) {
        Rid rid = scan.rid();
        auto rec = fh->get_record(rid, context);
        char *key = new char[col_tot_len];
        int offset = 0;
        for (auto &col : index_cols) {
            memcpy(key + offset, rec->data + col.offset, col.len);
            offset += col.len;
        }
        if (ih->insert_entry(key, rid, context->txn_) == -1) {
            delete[] key;
            throw InternalError("Duplicate key for unique index");
        }
        delete[] key;
        scan.next();
    }
    IndexMeta im;
    im.cols = index_cols;
    im.col_num = (int)col_names.size();
    im.col_tot_len = col_tot_len;
    im.tab_name = tab_name;
    tab.indexes.push_back(im);
    for (auto &col : tab.cols) {
        for (auto &idx_col : index_cols) {
            if (col.name == idx_col.name) {
                col.index = true;
            }
        }
    }
    flush_meta();
}

void SmManager::drop_index(const string &tab_name, const vector<string> &col_names, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (!tab.is_index(col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    vector<ColMeta> index_cols;
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_cols.push_back(*col);
    }
    std::string ix_name = ix_manager_->get_index_name(tab_name, index_cols);
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_[ix_name].get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, index_cols);
    for (auto it = tab.indexes.begin(); it != tab.indexes.end(); ++it) {
        if (it->col_num == (int)col_names.size()) {
            bool match = true;
            for (int i = 0; i < it->col_num; i++) {
                if (it->cols[i].name != col_names[i]) { match = false; break; }
            }
            if (match) {
                tab.indexes.erase(it);
                break;
            }
        }
    }
    for (auto &col : tab.cols) {
        for (auto &idx_col : index_cols) {
            if (col.name == idx_col.name) {
                col.index = false;
            }
        }
    }
    flush_meta();
}

void SmManager::drop_index(const string &tab_name, const vector<ColMeta> &cols, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::string ix_name = ix_manager_->get_index_name(tab_name, cols);
    if (ihs_.count(ix_name)) {
        ix_manager_->close_index(ihs_[ix_name].get());
        ihs_.erase(ix_name);
    }
    ix_manager_->destroy_index(tab_name, cols);
    flush_meta();
}

void SmManager::show_index(const string &tab_name, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);
    RecordPrinter rec_printer(3);
    rec_printer.print_separator(context);
    rec_printer.print_record({"Table", "Non_unique", "Key_name"}, context);
    rec_printer.print_separator(context);
    for (auto &index : tab.indexes) {
        string col_list = "(";
        for (size_t i = 0; i < index.cols.size(); i++) {
            if (i > 0) col_list += ",";
            col_list += index.cols[i].name;
        }
        col_list += ")";
        rec_printer.print_record({tab_name, "unique", col_list}, context);
    }
    rec_printer.print_separator(context);
}
