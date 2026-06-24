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

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int num = get_size();
    int lo = 0, hi = num;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int num = get_size();
    int lo = 0, hi = num;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // 内部节点的第0个key是不用于比较的占位符，从1开始
    int pos = upper_bound(key);  // pos is in [1, num_key)
    if (pos == 0) {
        pos = 1;
    }
    // 小于key的最大key所在位置是pos-1，对应的孩子是pos-1
    // 实际上，内部节点的查找逻辑是：找到第一个大于key的key_idx，
    // 然后取key_idx-1处的rid作为孩子节点
    return value_at(pos - 1);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    int num = get_size();
    // 将[pos, num)的键值对后移n位
    if (pos < num) {
        // 从后往前移动
        memmove(keys + (pos + n) * file_hdr->col_tot_len_,
                keys + pos * file_hdr->col_tot_len_,
                (num - pos) * file_hdr->col_tot_len_);
        memmove(rids + (pos + n),
                rids + pos,
                (num - pos) * sizeof(Rid));
    }
    // 复制新的键值对
    memcpy(keys + pos * file_hdr->col_tot_len_, key, n * file_hdr->col_tot_len_);
    memcpy(rids + pos, rid, n * sizeof(Rid));
    set_size(num + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    // 如果key已存在，不插入
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return get_size();
    }
    insert_pairs(pos, key, &value, 1);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 */
void IxNodeHandle::erase_pair(int pos) {
    int num = get_size();
    if (pos >= num) return;
    // 将[pos+1, num)的键值对前移
    if (pos + 1 < num) {
        memmove(keys + pos * file_hdr->col_tot_len_,
                keys + (pos + 1) * file_hdr->col_tot_len_,
                (num - pos - 1) * file_hdr->col_tot_len_);
        memmove(rids + pos,
                rids + (pos + 1),
                (num - pos - 1) * sizeof(Rid));
    }
    set_size(num - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < get_size() && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return get_size();
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    if (is_empty()) {
        return std::make_pair(nullptr, false);
    }
    page_id_t page_no = file_hdr_->root_page_;
    auto node = fetch_node(page_no);
    
    while (!node->is_leaf_page()) {
        page_id_t child_page = node->internal_lookup(key);
        auto child = fetch_node(child_page);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = child;
    }
    
    return std::make_pair(node, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    if (is_empty()) return false;
    
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, transaction, false);
    if (leaf == nullptr) return false;
    
    Rid *value = nullptr;
    bool found = leaf->leaf_lookup(key, &value);
    if (found && value != nullptr) {
        result->push_back(*value);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    auto new_node = create_node();
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->page_hdr->parent;
    
    int total = node->get_size();
    int left_size = total / 2;
    int right_size = total - left_size;
    
    // 将右半部分移动到new_node
    memcpy(new_node->keys, node->get_key(left_size), right_size * file_hdr_->col_tot_len_);
    memcpy(new_node->rids, node->get_rid(left_size), right_size * sizeof(Rid));
    new_node->set_size(right_size);
    node->set_size(left_size);
    
    if (node->is_leaf_page()) {
        // 更新叶子链表指针
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());
        
        if (new_node->get_next_leaf() != IX_NO_PAGE) {
            auto next = fetch_node(new_node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        
        // 更新file_hdr中的最右叶子
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // 非叶子节点：更新new_node所有孩子的父指针
        maintain_child(new_node, -1);  // update all children
        // 更新node的所有孩子父指针
        maintain_child(node, -1);
    }
    
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    // 如果旧节点是根节点，创建新的根节点
    if (old_node->is_root_page()) {
        auto new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->set_size(1);
        // 第一个key是占位符，第二个key指向new_node
        // 新根节点的第一个child指向old_node
        new_root->set_key(0, old_node->get_key(0));
        new_root->set_rid(0, Rid{old_node->get_page_no(), -1});
        
        // 将key和new_node插入到新根节点
        // 使用insert_pairs插入在位置1
        Rid new_rid_val = {new_node->get_page_no(), -1};
        new_root->insert_pairs(1, key, &new_rid_val, 1);
        
        file_hdr_->root_page_ = new_root->get_page_no();
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }
    
    // 获取父节点
    auto parent = fetch_node(old_node->get_parent_page_no());
    
    // 找到old_node在parent中的位置
    int child_idx = parent->find_child(old_node);
    
    // 在parent中child_idx之后插入(key, new_node_page_no)
    // key是new_node的第一个有效key
    Rid new_rid = {new_node->get_page_no(), -1};
    parent->insert_pairs(child_idx + 1, key, &new_rid, 1);
    
    if (parent->get_size() >= parent->get_max_size()) {
        // 父节点满了，分裂
        auto new_parent = split(parent);
        // 取new_parent的第一个key用于向上插入
        const char *up_key = new_parent->get_key(0);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        insert_into_parent(parent, up_key, new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    } else {
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    }
}

/**
 * @brief 用于在B+树中插入键值对
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    if (is_empty()) {
        // 创建第一个叶子节点作为根节点
        auto root = create_node();
        root->page_hdr->is_leaf = true;
        root->set_size(1);
        root->set_key(0, key);
        root->set_rid(0, value);
        file_hdr_->root_page_ = root->get_page_no();
        file_hdr_->first_leaf_ = root->get_page_no();
        file_hdr_->last_leaf_ = root->get_page_no();
        page_id_t leaf_page = root->get_page_no();
        buffer_pool_manager_->unpin_page(root->get_page_id(), true);
        return leaf_page;
    }
    
    auto [leaf, root_latched] = find_leaf_page(key, Operation::INSERT, transaction, false);
    if (leaf == nullptr) {
        return -1;
    }
    
    // 检查键是否已存在（唯一索引）
    int pos = leaf->lower_bound(key);
    if (pos < leaf->get_size() && 
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) == 0) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return -1;  // key already exists, unique index violation
    }
    
    leaf->insert(key, value);
    page_id_t leaf_page = leaf->get_page_no();
    
    if (leaf->get_size() >= leaf->get_max_size()) {
        auto new_leaf = split(leaf);
        // 取new_leaf的第一个key向上插入
        const char *up_key = new_leaf->get_key(0);
        insert_into_parent(leaf, up_key, new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }
    
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return leaf_page;
}

/**
 * @brief 在B+树中删除一个键值对
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    if (is_empty()) return false;
    
    auto [leaf, root_latched] = find_leaf_page(key, Operation::DELETE, transaction, false);
    if (leaf == nullptr) return false;
    
    int pos = leaf->lower_bound(key);
    if (pos >= leaf->get_size() || 
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) != 0) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return false;
    }
    
    leaf->erase_pair(pos);
    
    bool node_deleted = coalesce_or_redistribute(leaf, transaction, nullptr);
    // If node was deleted by adjust_root, don't unpin it again
    if (!node_deleted) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    
    if (node->get_size() >= node->get_min_size()) {
        return false;  // no action needed
    }
    
    auto parent = fetch_node(node->get_parent_page_no());
    int child_idx = parent->find_child(node);
    
    // 优先选择前驱兄弟节点
    int neighbor_idx = (child_idx > 0) ? (child_idx - 1) : (child_idx + 1);
    auto neighbor = fetch_node(parent->value_at(neighbor_idx));
    
    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        // 重新分配
        redistribute(neighbor, node, parent, child_idx);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return false;
    } else {
        // 合并
        bool ret = coalesce(&neighbor, &node, &parent, child_idx, transaction, root_is_latched);
        // coalesce may recursively delete parent; only unpin if still valid
        if (neighbor != nullptr) {
            buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        }
        // parent is already handled inside coalesce (deleted or unpinned), skip here
        return ret;
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 内部节点，只需要一个孩子，把它提升为根
        page_id_t child_page = old_root_node->value_at(0);
        auto child = fetch_node(child_page);
        child->set_parent_page_no(IX_NO_PAGE);
        file_hdr_->root_page_ = child_page;
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        
        // 删除旧的根页面
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        return true;
    }
    
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 叶子根节点为空，树变为空
        file_hdr_->root_page_ = IX_NO_PAGE;
        file_hdr_->first_leaf_ = IX_NO_PAGE;
        file_hdr_->last_leaf_ = IX_NO_PAGE;
        buffer_pool_manager_->delete_page(old_root_node->get_page_id());
        return true;
    }
    
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index == 0) {
        // node在左边，neighbor在右边，从neighbor移第一个键值对到node末尾
        node->insert_pairs(node->get_size(), neighbor_node->get_key(0), neighbor_node->get_rid(0), 1);
        neighbor_node->erase_pair(0);
        
        // 更新parent中指向neighbor的key
        parent->set_key(index + 1, neighbor_node->get_key(0));
        
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    } else {
        // neighbor在左边，node在右边，从neighbor移最后一个键值对到node开头
        int last = neighbor_node->get_size() - 1;
        node->insert_pairs(0, neighbor_node->get_key(last), neighbor_node->get_rid(last), 1);
        neighbor_node->erase_pair(last);
        
        // 更新parent中指向node的key
        parent->set_key(index, node->get_key(0));
        
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    }
}

/**
 * @brief 合并(Coalesce)函数
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 确保neighbor在左，node在右
    if (index == 0) {
        std::swap(*neighbor_node, *node);
        index = 1;
    }
    
    IxNodeHandle *left = *neighbor_node;
    IxNodeHandle *right = *node;
    
    // 将right的所有键值对移到left
    left->insert_pairs(left->get_size(), right->get_key(0), right->get_rid(0), right->get_size());
    
    if (left->is_leaf_page()) {
        // 更新叶子链表
        left->set_next_leaf(right->get_next_leaf());
        if (right->get_next_leaf() != IX_NO_PAGE) {
            auto next = fetch_node(right->get_next_leaf());
            next->set_prev_leaf(left->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (file_hdr_->last_leaf_ == right->get_page_no()) {
            file_hdr_->last_leaf_ = left->get_page_no();
        }
    } else {
        maintain_child(left, -1);
    }
    
    // 从parent中删除指向right的条目
    (*parent)->erase_pair(index);
    
    // 删除right节点
    buffer_pool_manager_->delete_page(right->get_page_id());
    
    // 递归处理parent
    return coalesce_or_redistribute(*parent, transaction, root_is_latched);
}

/**
 * @brief 这里把iid转换成了rid
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return rid;
}

/**
 * @brief FindLeafPage + lower_bound
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    if (is_empty()) {
        return leaf_end();
    }
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr, false);
    if (leaf == nullptr) {
        return leaf_end();
    }
    int slot = leaf->lower_bound(key);
    if (slot >= leaf->get_size()) {
        // 大于该叶子的所有key，移到下一个叶子
        if (leaf->get_next_leaf() != IX_NO_PAGE) {
            page_id_t next_page = leaf->get_next_leaf();
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
            return Iid{next_page, 0};
        } else {
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
            return leaf_end();
        }
    }
    Iid result = {leaf->get_page_no(), slot};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return result;
}

/**
 * @brief FindLeafPage + upper_bound
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    if (is_empty()) {
        return leaf_end();
    }
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr, false);
    if (leaf == nullptr) {
        return leaf_end();
    }
    int slot = leaf->upper_bound(key);
    if (slot >= leaf->get_size()) {
        if (leaf->get_next_leaf() != IX_NO_PAGE) {
            page_id_t next_page = leaf->get_next_leaf();
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
            return Iid{next_page, 0};
        } else {
            buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
            return leaf_end();
        }
    }
    Iid result = {leaf->get_page_no(), slot};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return result;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 */
Iid IxIndexHandle::leaf_end() const {
    if (file_hdr_->last_leaf_ == IX_NO_PAGE) {
        return Iid{-1, -1};
    }
    auto last = fetch_node(file_hdr_->last_leaf_);
    Iid result = {last->get_page_no(), last->get_size()};
    buffer_pool_manager_->unpin_page(last->get_page_id(), false);
    return result;
}

/**
 * @brief 指向第一个叶子的第一个结点
 */
Iid IxIndexHandle::leaf_begin() const {
    if (file_hdr_->first_leaf_ == IX_NO_PAGE) {
        return Iid{-1, -1};
    }
    return Iid{file_hdr_->first_leaf_, 0};
}

/**
 * @brief 用于获取指定页面的节点句柄
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    auto page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    auto node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 在B+树中创建一个新的节点
 */
IxNodeHandle *IxIndexHandle::create_node() {
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    auto new_page = buffer_pool_manager_->new_page(&new_page_id);
    auto node = new IxNodeHandle(file_hdr_, new_page);
    // 初始化页面头部
    node->page_hdr->num_key = 0;
    node->page_hdr->is_leaf = false;
    node->page_hdr->parent = IX_NO_PAGE;
    node->page_hdr->next_leaf = IX_NO_PAGE;
    node->page_hdr->prev_leaf = IX_NO_PAGE;
    return node;
}

/**
 * @brief 维护孩子节点的父指针
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (node->is_leaf_page()) return;
    
    if (child_idx == -1) {
        // 更新所有孩子
        for (int i = 0; i < node->get_size(); i++) {
            auto child = fetch_node(node->value_at(i));
            child->set_parent_page_no(node->get_page_no());
            buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        }
    } else {
        auto child = fetch_node(node->value_at(child_idx));
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}

/**
 * @brief 删除叶子节点（更新前后指针）
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    if (leaf->get_prev_leaf() != IX_NO_PAGE) {
        auto prev = fetch_node(leaf->get_prev_leaf());
        prev->set_next_leaf(leaf->get_next_leaf());
        buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    } else {
        file_hdr_->first_leaf_ = leaf->get_next_leaf();
    }
    
    if (leaf->get_next_leaf() != IX_NO_PAGE) {
        auto next = fetch_node(leaf->get_next_leaf());
        next->set_prev_leaf(leaf->get_prev_leaf());
        buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    } else {
        file_hdr_->last_leaf_ = leaf->get_prev_leaf();
    }
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    // only unpin, dirty flag should be set by caller
}