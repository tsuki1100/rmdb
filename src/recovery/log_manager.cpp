/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "log_manager.h"

lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    lsn_t lsn = global_lsn_.fetch_add(1);
    log_record->lsn_ = lsn;
    int append_size = log_record->log_tot_len_;
    if (log_buffer_.is_full(append_size)) {
        flush_log_to_disk();
    }
    char* dest = log_buffer_.buffer_ + log_buffer_.offset_;
    log_record->serialize(dest);
    log_buffer_.offset_ += append_size;
    return lsn;
}

void LogManager::flush_log_to_disk() {
    if (log_buffer_.offset_ == 0) return;
    // Use direct file descriptor to avoid DiskManager tracking issues
    int fd = log_fd_;
    if (fd < 0) {
        // First time: open with POSIX open (not DiskManager)
        fd = open("db.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) return;
        log_fd_ = fd;
    }
    ssize_t written = write(fd, log_buffer_.buffer_, log_buffer_.offset_);
    (void)written;
    persist_lsn_ = global_lsn_.load();
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, LOG_BUFFER_SIZE + 1);
}
