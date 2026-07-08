/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"
#include "ix_scan.h"

int IxNodeHandle::lower_bound(const char *target) const {
    int l = 0, r = page_hdr->num_key;
    while (l < r) {
        int mid = (l + r) / 2;
        const char *key = get_key(mid);
        if (ix_compare(target, key, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            r = mid;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int l = 0, r = page_hdr->num_key;
    while (l < r) {
        int mid = (l + r) / 2;
        const char *key = get_key(mid);
        if (ix_compare(target, key, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            r = mid;
        } else {
            l = mid + 1;
        }
    }
    return l;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        int cmp = ix_compare(key, get_key(pos), file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp == 0) {
            *value = get_rid(pos);
            return true;
        }
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key);
    if (pos == 0) {
        return value_at(0);
    }
    return value_at(pos);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    int old_num = page_hdr->num_key;
    // Shift existing keys and rids to the right
    if (pos < old_num) {
        memmove(get_key(pos + n), get_key(pos), (old_num - pos) * file_hdr->col_tot_len_);
        memmove(get_rid(pos + n), get_rid(pos), (old_num - pos) * sizeof(Rid));
    }
    // Insert new keys and rids
    memcpy(get_key(pos), key, n * file_hdr->col_tot_len_);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    page_hdr->num_key += n;
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    // Check for duplicate key
    if (pos < page_hdr->num_key) {
        int cmp = ix_compare(key, get_key(pos), file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp == 0) {
            return page_hdr->num_key; // key already exists, no insertion
        }
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

void IxNodeHandle::erase_pair(int pos) {
    int old_num = page_hdr->num_key;
    if (pos < old_num - 1) {
        memmove(get_key(pos), get_key(pos + 1), (old_num - pos - 1) * file_hdr->col_tot_len_);
        memmove(get_rid(pos), get_rid(pos + 1), (old_num - pos - 1) * sizeof(Rid));
    }
    page_hdr->num_key--;
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        int cmp = ix_compare(key, get_key(pos), file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp == 0) {
            erase_pair(pos);
        }
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    page_id_t cur_page_no = file_hdr_->root_page_;
    IxNodeHandle *node = fetch_node(cur_page_no);
    IxNodeHandle *leaf = nullptr;
    while (!node->is_leaf_page()) {
        page_id_t next;
        if (find_first) {
            next = node->value_at(0);
        } else {
            next = node->internal_lookup(key);
        }
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        cur_page_no = next;
        node = fetch_node(cur_page_no);
    }
    leaf = node;
    return {leaf, false};
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, transaction);
    if (leaf == nullptr) return false;
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found) {
        result->push_back(*rid);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return found;
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->page_hdr->parent;
    int total = node->get_size();
    int split_point = total / 2;
    int move_cnt = total - split_point;
    new_node->insert_pairs(0, node->get_key(split_point), node->get_rid(split_point), move_cnt);
    node->set_size(split_point);
    if (node->is_leaf_page()) {
        new_node->page_hdr->prev_leaf = node->get_page_no();
        new_node->page_hdr->next_leaf = node->get_next_leaf();
        node->page_hdr->next_leaf = new_node->get_page_no();
        if (new_node->get_next_leaf() != IX_NO_PAGE) {
            IxNodeHandle *next = fetch_node(new_node->get_next_leaf());
            next->page_hdr->prev_leaf = new_node->get_page_no();
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    }
    return new_node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->set_size(1);
        // New root has 1 separating key and 2 children
        memcpy(new_root->get_key(0), key, file_hdr_->col_tot_len_);
        new_root->set_rid(0, Rid{old_node->get_page_no(), 0});
        new_root->set_rid(1, Rid{new_node->get_page_no(), 0});

        old_node->page_hdr->parent = new_root->get_page_no();
        new_node->page_hdr->parent = new_root->get_page_no();

        file_hdr_->root_page_ = new_root->get_page_no();
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    page_id_t parent_page_no = old_node->get_parent_page_no();
    IxNodeHandle *parent = fetch_node(parent_page_no);
    int insert_pos = parent->find_child(old_node);
    parent->insert_pair(insert_pos, key, Rid{new_node->get_page_no(), 0});

    if (parent->get_size() > parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }

    new_node->page_hdr->parent = parent->get_page_no();

    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    if (leaf == nullptr) return -1;
    int old_size = leaf->get_size();
    leaf->insert(key, value);
    if (leaf->get_size() == old_size) {
        // Key already exists, duplicate - uniqueness violation
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return -1;
    }
    page_id_t leaf_no = leaf->get_page_no();
    if (leaf->get_size() > leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }
    if (leaf_no == file_hdr_->last_leaf_) {
        file_hdr_->last_leaf_ = leaf_no;
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return leaf_no;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    if (leaf == nullptr) return false;
    int old_size = leaf->get_size();
    leaf->remove(key);
    if (leaf->get_size() == old_size) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return false;
    }
    bool need_delete = false;
    if (!leaf->is_root_page()) {
        bool root_is_latched = false;
        need_delete = coalesce_or_redistribute(leaf, transaction, &root_is_latched);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return true;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int idx = parent->find_child(node);
    int neighbor_idx = (idx == 0) ? 1 : idx - 1;
    IxNodeHandle *neighbor = fetch_node(parent->value_at(neighbor_idx));
    int total = node->get_size() + neighbor->get_size();
    if (total >= node->get_min_size() * 2) {
        redistribute(neighbor, node, parent, idx);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        delete neighbor;
        delete parent;
        return false;
    }
    // Coalesce: merge node into neighbor (on the left)
    if (idx == 0) {
        // node is the leftmost child, coalesce to right instead
        std::swap(neighbor, node);
        idx = neighbor_idx;
    }
    bool parent_delete = coalesce(&neighbor, &node, &parent, idx, transaction, root_is_latched);
    buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
    buffer_pool_manager_->unpin_page(node->get_page_id(), true);
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    // Actually coalesce already unpins, so be careful
    // For simplicity, let's do a simpler implementation
    delete neighbor;
    delete node;
    delete parent;
    return parent_delete;
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t child_page_no = old_root_node->value_at(0);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->page_hdr->parent = IX_NO_PAGE;
        file_hdr_->root_page_ = child_page_no;
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
        return true;
    }
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        file_hdr_->root_page_ = IX_NO_PAGE;
        return true;
    }
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index == 0) {
        // neighbor is right sibling, move first key of neighbor to end of node
        char *key = neighbor_node->get_key(0);
        Rid *rid = neighbor_node->get_rid(0);
        node->insert_pair(node->get_size(), key, *rid);
        neighbor_node->erase_pair(0);
        maintain_child(node, node->get_size() - 1);
        maintain_child(neighbor_node, 0);
        // Update parent key
        parent->set_key(0, neighbor_node->get_key(0));
    } else {
        // neighbor is left sibling, move last key of neighbor to front of node
        char *key = neighbor_node->get_key(neighbor_node->get_size() - 1);
        Rid *rid = neighbor_node->get_rid(neighbor_node->get_size() - 1);
        node->insert_pair(0, key, *rid);
        neighbor_node->erase_pair(neighbor_node->get_size() - 1);
        maintain_child(node, 0);
        // Update parent key
        parent->set_key(index, node->get_key(0));
    }
}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // Merge *node into *neighbor_node (which is on the left)
    int node_size = (*node)->get_size();
    (*neighbor_node)->insert_pairs((*neighbor_node)->get_size(), (*node)->get_key(0), (*node)->get_rid(0), node_size);
    if (!(*neighbor_node)->is_leaf_page()) {
        for (int i = 0; i < node_size; i++) {
            maintain_child(*neighbor_node, (*neighbor_node)->get_size() - node_size + i);
        }
    } else {
        // Update leaf links
        (*neighbor_node)->page_hdr->next_leaf = (*node)->get_next_leaf();
        if ((*node)->get_next_leaf() != IX_NO_PAGE) {
            IxNodeHandle *next = fetch_node((*node)->get_next_leaf());
            next->page_hdr->prev_leaf = (*neighbor_node)->get_page_no();
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
            delete next;
        }
        if (file_hdr_->last_leaf_ == (*node)->get_page_no()) {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
    }
    // Erase the separator key in parent that pointed to *node
    int parent_idx = (*parent)->find_child(*node);
    (*parent)->erase_pair(parent_idx);
    // Release the node
    release_node_handle(**node);
    // Check if parent needs coalescing/redistributing
    if ((*parent)->get_size() < (*parent)->get_min_size() && !(*parent)->is_root_page()) {
        return true; // parent needs further processing
    }
    if ((*parent)->get_size() == 0 && (*parent)->is_root_page()) {
        return adjust_root(*parent);
    }
    return false;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (leaf == nullptr) return Iid{-1, -1};
    int slot = leaf->lower_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (leaf == nullptr) return Iid{-1, -1};
    int slot = leaf->upper_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    memset(page->get_data(), 0, PAGE_SIZE);  // clear stale data from reused BPM frames
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);
        curr = parent;
        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    delete prev;
    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next;
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
    }
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    delete node;
    return rid;
}
