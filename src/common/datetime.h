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

#include <cstdint>
#include <string>

// DATETIME validation and packing utilities
// Stored as int64_t: YYYYMMDDHHMMSS for direct comparison

inline bool is_leap_year(int year) {
    return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

inline int days_in_month(int year, int month) {
    static const int dpm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return dpm[month - 1];
}

// Parse 'YYYY-MM-DD HH:MM:SS' into packed int64_t YYYYMMDDHHMMSS
// Returns true on success, false on invalid input
inline bool parse_datetime(const std::string &str, int64_t &result) {
    // Must be exactly 19 chars: YYYY-MM-DD HH:MM:SS
    if (str.length() != 19) return false;
    if (str[4] != '-' || str[7] != '-' || str[10] != ' '
        || str[13] != ':' || str[16] != ':') return false;

    for (int i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (str[i] < '0' || str[i] > '9') return false;
    }

    int year = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day = std::stoi(str.substr(8, 2));
    int hour = std::stoi(str.substr(11, 2));
    int minute = std::stoi(str.substr(14, 2));
    int second = std::stoi(str.substr(17, 2));

    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1) return false;
    int max_day = days_in_month(year, month);
    if (day > max_day) return false;
    if (hour < 0 || hour > 23) return false;
    if (minute < 0 || minute > 59) return false;
    if (second < 0 || second > 59) return false;

    result = (int64_t)year * 10000000000LL
           + (int64_t)month * 100000000
           + (int64_t)day * 1000000
           + (int64_t)hour * 10000
           + (int64_t)minute * 100
           + (int64_t)second;
    return true;
}

// Format packed int64_t YYYYMMDDHHMMSS back to 'YYYY-MM-DD HH:MM:SS'
inline std::string format_datetime(int64_t packed) {
    int year = (int)(packed / 10000000000LL);
    int month = (int)((packed / 100000000) % 100);
    int day = (int)((packed / 1000000) % 100);
    int hour = (int)((packed / 10000) % 100);
    int minute = (int)((packed / 100) % 100);
    int second = (int)(packed % 100);

    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return std::string(buf);
}
