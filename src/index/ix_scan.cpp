/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2. ... */

#include "ix_scan.h"

void IxScan::next() {
    assert(!is_end());
    IxNodeHandle *node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    assert(iid_.slot_no < node->get_size());
    iid_.slot_no++;
    if (iid_.page_no != ih_->file_hdr_->last_leaf_ && iid_.slot_no == node->get_size()) {
        iid_.slot_no = 0;
        iid_.page_no = node->get_next_leaf();
    }
    bpm_->unpin_page(node->get_page_id(), false);
    delete node;
}

Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}
