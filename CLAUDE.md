# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RMDB is a C++17 relational database system — an educational project from Renmin University of China. It uses a **multi-threaded client-server** architecture over TCP port 8765, with Flex/Bison SQL parsing, a volcano-style query executor, B+ tree indexes, 2PL concurrency control, and ARIES-based WAL recovery.

Target environment: **Ubuntu 18.04+ (x86_64)**, GCC ≥ 7.1, CMake ≥ 3.16. Development typically done via VS Code SSH into a Linux VM or WSL2.

## Build

```sh
# Server
mkdir build && cd build && cmake .. && make -j$(nproc)

# Client (separate project)
cd rmdb_client && mkdir build && cd build && cmake .. && make -j$(nproc)
```

Dependencies: `build-essential`, `cmake` (≥3.16), `flex`, `bison`, `libreadline-dev`. Google Test is vendored under `deps/googletest/`.

Release build: add `-DCMAKE_BUILD_TYPE=Release` to the cmake line (uses `-Wall -O3` instead of `-Wall -O0 -g -ggdb3`).

Regenerating parser sources after editing `lex.l` or `yacc.y`:
```sh
flex --header-file=lex.yy.hpp -o lex.yy.cpp lex.l
bison --defines=yacc.tab.hpp -o yacc.tab.cpp yacc.y
```

## Run

```sh
./build/bin/rmdb <database_name>   # server — creates db directory if new
./rmdb_client/build/rmdb_client    # client — defaults to 127.0.0.1:8765
./build/bin/unit_test              # Google Test suite
./build/bin/test_parser            # standalone parser test (CMake CTest target)
```

Client options: `-h <host>` `-p <port>` `-s <unix_socket_path>`. Exit with `exit;` or `Ctrl+C` (client). Server stops on `Ctrl+C`.

## Architecture

### SQL query pipeline

```
Client socket → yyparse (lex.l + yacc.y) → AST (ast.h)
  → Analyze::do_analyze() (semantic analysis, query rewriting)
  → Optimizer::plan_query() → Plan tree (ScanPlan/JoinPlan/SortPlan/ProjectionPlan)
  → Portal::start() → executor tree (AbstractExecutor hierarchy)
  → Portal::run() → QlManager → volcano nextTuple() iteration → result sent to client
  → Portal::drop() — cleanup
```

For DDL and transaction commands, Portal bypasses the executor and calls `SmManager`/`TransactionManager` directly.

### Subsystem layers

| Layer | Dir | Library | Purpose |
|-------|-----|---------|---------|
| **Storage** | `src/storage/` | `storage` (static) | `DiskManager` — raw file I/O, fd management, `fd2pageno_` per-file page counter. `BufferPoolManager` — in-memory page cache, 256MB = 65536 frames × 4KB pages, per-frame `pin_count_`, dirty flag, RWLatch. Includes `LRUReplacer`. |
| **Replacer** | `src/replacer/` | `lru_replacer` (static) | `LRUReplacer` — `LRUlist_` + `LRUhash_` for O(1) victim selection. |
| **Record** | `src/record/` | `record` (static) | `RmManager` / `RmFileHandle` / `RmScan`. Heap-file: each page has header + bitmap (free-slot tracking) + slot array. `RmFileHdr` stores `record_size`, `num_pages`, `num_records_per_page`, `first_free_page_no`, `bitmap_size`. |
| **Index** | `src/index/` | `index` (static) | B+ tree: `IxManager` / `IxIndexHandle` / `IxScan`. `IxFileHdr` holds `root_page_`, `first_free_page_no_`, `col_num_`, `col_types_`/`col_lens_`, `btree_order_`, `first_leaf_`/`last_leaf_`. `IxPageHdr`: `parent`, `num_key`, `is_leaf`, `prev_leaf`/`next_leaf`. Internal nodes store keys + child pointers; leaf nodes store keys + RIDs. |
| **System** | `src/system/` | `system` (static) | `SmManager` — DDL execution and metadata. `DbMeta` persisted to `db.meta` file, loaded at `open_db()`. Holds `TabMeta` (name, `vector<ColMeta>`, `vector<IndexMeta>`) and open file handles (`fhs_`, `ihs_`). |
| **Transaction** | `src/transaction/` | `transaction` (static) | `TransactionManager` — 2PL-based, global `txn_map`, transaction lifecycle (begin/commit/abort). `LockManager` — lock table keyed by `LockDataId{fd, Rid, type}`, per-lock request queue. |
| **Recovery** | `src/recovery/` | `recovery` (static) | ARIES-inspired WAL: `LogManager` — `LogBuffer` (4MB), `global_lsn_`, `flush_log_to_disk()`. Log record types: Begin, Insert, Delete, Update, Commit, Abort. `RecoveryManager` — startup sequence: `analyze()` → `redo()` → `undo()`. WAL rule: `persist_lsn_` ≥ page's `page_lsn_` before writing page to disk. |
| **Parser** | `src/parser/` | `parser` (static) | Flex `lex.l` + Bison `yacc.y` → AST nodes in `ast.h` (SelectStmt, InsertStmt, UpdateStmt, DeleteStmt, etc.). |
| **Analyze** | `src/analyze/` | `analyze` (static) | Semantic analysis: resolves column references against `DbMeta`, produces `Query` objects. |
| **Optimizer** | `src/optimizer/` | `planner` (static) | `Planner` — builds `ScanPlan` (seq/index), `JoinPlan` (nested-loop), `SortPlan`, `ProjectionPlan`. `Optimizer::plan_query()` selects index scan over seq scan when applicable. |
| **Execution** | `src/execution/` | `execution` (static) | Volcano-style (iterator model) executors: `SeqScanExecutor`, `IndexScanExecutor`, `NestedLoopJoinExecutor`, `ProjectionExecutor`, `SortExecutor`, `InsertExecutor`, `UpdateExecutor`, `DeleteExecutor`. `QlManager` dispatches: `select_from()`, `run_dml()`, `run_mutli_query()`, `run_cmd_utility()`. |
| **Portal** | `src/portal.h` | (header-only) | `Plan` → executor tree conversion, dispatches by `portalTag`: `PORTAL_ONE_SELECT`, `PORTAL_DML_WITHOUT_SELECT`, `PORTAL_MULTI_QUERY`, `PORTAL_CMD_UTILITY`. |
| **Common** | `src/common/` | (headers) | `Value` (tagged union: INT/FLOAT/STRING + raw `RmRecord`), `Condition`, `TabCol`, `SetClause`, `CompOp`. Config constants in `config.h`: `PAGE_SIZE=4096`, `BUFFER_POOL_SIZE=65536`, `LOG_BUFFER_SIZE=4MB`, `BUFFER_LENGTH=8192`. |

### Library dependency graph

```
rmdb (binary)
  ├── parser       (Flex/Bison SQL parser + AST)
  ├── execution    → system, record, transaction
  ├── planner      (standalone, used by Optimizer)
  └── analyze      (semantic analysis, depends on parser AST types)

execution → system → index → storage (includes replacer)
          → record → system, transaction, storage
          → transaction → system, recovery → system

unit_test (binary) → storage, lru_replacer, record, gtest_main
```

Note: `record` and `system` have a mutual dependency — `record` links `system`, and `system` links `record`.

### Key data structures

- **`PageId`** = `{int fd, page_id_t page_no}` — globally unique page identifier
- **`Rid`** = `{int page_no, int slot_no}` — record location within a table heap file
- **`TabCol`** = `{string tab_name, string col_name}` — qualified column reference
- **`Value`** = `{ColType type; union{int, float}; string str_val; shared_ptr<RmRecord> raw}` — typed value, serialized to `RmRecord` for storage
- **`Condition`** = `{TabCol lhs; CompOp op; bool is_rhs_val; TabCol/Value rhs}` — WHERE/JOIN predicate
- **`SetClause`** = `{TabCol lhs; Value rhs}` — SET column = value for UPDATE
- **`ColMeta`** = `{tab_name, name, ColType type, int len, int offset, bool index}` — column metadata in `TabMeta`
- **`PlanTag`** enum — all plan node types: T_SeqScan, T_IndexScan, T_NestLoop, T_Sort, T_Projection, T_Insert, T_Update, T_Delete, T_select, plus DDL and transaction tags
- **`RmFileHdr`** — file header page: `record_size`, `num_pages`, `num_records_per_page`, `first_free_page_no`, `bitmap_size`
- **`IxFileHdr`** — B+ tree metadata: `root_page_`, `col_num_`, `col_types_`/`col_lens_`, `col_tot_len_`, `btree_order_`, `first_leaf_`/`last_leaf_`

### Supported SQL features

- **Types**: INT, BIGINT, FLOAT, CHAR(n), DATETIME (format: `YYYY-MM-DD HH:MM:SS`)
- **DDL**: CREATE/DROP TABLE, CREATE/DROP INDEX, SHOW TABLES, DESC TABLE
- **DML**: INSERT, UPDATE, DELETE, SELECT with WHERE, projection, table aliases
- **Join**: `FROM t1, t2 WHERE t1.x = t2.y` and `FROM t1 JOIN t2 ON t1.x = t2.y` (nested-loop)
- **Aggregates**: SUM, MAX, MIN, COUNT(column), COUNT(*)
- **Sorting**: ORDER BY column [ASC|DESC], LIMIT n
- **Transactions**: BEGIN, COMMIT, ABORT (ROLLBACK) — explicit multi-statement and implicit auto-commit for single statements

### Server lifecycle (`src/rmdb.cpp`)

1. `SmManager::open_db(db_name)` — loads `db.meta` into `DbMeta`, opens all table/index files
2. `RecoveryManager::analyze()` → `redo()` → `undo()` — crash recovery from `db.log`
3. `start_server()` — binds TCP 8765, accept loop spawning one `pthread` per client
4. Per-request in `client_handler()`: parse → analyze → optimize → portal start → portal run → auto-commit (if not explicit txn)
5. On SIGINT: `LogManager::flush_log_to_disk()`, `SmManager::close_db()`

Global singletons created in `main()`: `DiskManager`, `BufferPoolManager`, `RmManager`, `IxManager`, `SmManager`, `LockManager`, `TransactionManager`, `QlManager`, `LogManager`, `RecoveryManager`, `Planner`, `Optimizer`, `Portal`, `Analyze`.

## Tests

Single Google Test binary: `src/unit_test.cpp`. Links `storage`, `lru_replacer`, `record`, `gtest_main`.

Test categories (from 测试说明文档.pdf):

| Category | Tests |
|----------|-------|
| Basic query | DDL (create/drop/show tables), DML (insert/update/delete/select), joins, BIGINT bounds |
| Storage | datetime insert/validation, index creation and point/range queries, index performance (index scan ≤ 70% of seq scan time for 3000 rows), single-attribute and multi-attribute indexes |
| Aggregate | SUM, MAX/MIN, COUNT(column), COUNT(*) |
| Order by | ORDER BY single/multiple columns, ASC/DESC, LIMIT |
| Join | `t1, t2 WHERE cond` and `t1 JOIN t2 ON cond` |
| Transaction | commit test, abort test, commit with index, abort with index |
| Concurrency | dirty write, dirty read, lost update, unrepeatable read, phantom read |
| Crash recovery | single-thread crash, multi-thread crash, crash with index, large data recovery |

Test fixtures create/delete a temp directory per test in `SetUp()`/`TearDown()`. Tests access private members via `#define private public` before includes.

Parser has a separate CTest target: `test_parser` (built from `src/parser/test_parser.cpp`).

Server-side test validation: expected output is compared against `output.txt` — the server writes query results (or `failure\n`) to this file.

## Implementation Progress (2026-07-07)

> **Environment note**: Initial development was done on Windows/MinGW, then migrated to WSL2 (Ubuntu). The `rmdb` server requires Linux (POSIX sockets); only `unit_test` builds on MinGW. All subsequent development should be done inside WSL2 at `~/rmdb/`, NOT under `/mnt/d/` (9P filesystem is too slow and causes subtle bugs).

### Task completion status

| Phase | Task | Status | Verified |
|-------|------|--------|----------|
| Phase 1 | Task 1 - Storage Foundation | ✅ Complete | 5/5 unit tests pass |
| Phase 2 | Task 2 - Query Execution | ✅ Complete | TP1-5 + JOIN ON all pass |
| Phase 3 | Task 3 - BIGINT | ✅ Complete | 14/14 SQL tests, task3 spec matches |
| Phase 3 | Task 4 - DATETIME | ✅ Complete | task4 spec matches |
| Phase 4 | Task 5 - B+ Tree Index | ✅ Complete | task5 spec passes |
| Phase 4 | Task 6 - Aggregate Functions | ✅ Complete | task6 spec matches |
| Phase 4 | Task 7 - ORDER BY + LIMIT | ✅ Complete | task7 spec matches |
| Phase 5 | Task 8 - Block NLJ | ✅ Complete | 64KB join buffer, equi + non-equi |
| Phase 6 | Task 9 - Transaction Control | ✅ Complete | abort rolls back, commit persists |
| Phase 6 | Task 10 - Concurrency (2PL) | ✅ Complete | 2PL + no-wait, regression passes |
| Phase 6 | Task 11 - Crash Recovery | ✅ Complete | WAL + analyze/redo/undo, regression passes |

### Phase 1 — Storage Foundation (Task 1) ✅

**Files modified:**
- `src/storage/disk_manager.cpp` — 6 methods: write_page, read_page, create_file, open_file, close_file, destroy_file
- `src/storage/buffer_pool_manager.cpp` — 8 methods: find_victim_page, update_page, fetch_page, unpin_page, flush_page, new_page, delete_page, flush_all_pages
- `src/replacer/lru_replacer.cpp` — 3 methods: victim, pin, unpin
- `src/record/rm_file_handle.cpp` — 8 methods: get_record, insert_record(x2), delete_record, update_record, fetch_page_handle, create_new_page_handle, create_page_handle, release_page_handle
- `src/record/rm_scan.cpp` — 3 methods: constructor, next, is_end
- `src/common/config.h` — added missing `#include <string>`
- `src/transaction/transaction.h` — added missing `#include <memory>`

**Bugs fixed during Phase 1:**
1. `O_BINARY` missing on MinGW: Files opened without `O_BINARY` trigger CR/LF text-mode translation, corrupting binary page data. Fix: add `| O_BINARY` to all `open()` calls.
2. BPM `find_victim_page` deadlock: Internal `std::scoped_lock` caused deadlock. Fix: remove lock (callers hold latch_).
3. `flush_all_pages` random-order writes: Fix: collect into vector, sort by page_no.
4. `destroy_file` wrong exception: Test expected `FileNotFoundError`. Fix: throw `FileNotFoundError(path)`.
5. `config.h` missing `<string>`: Fix: add `#include <string>`.
6. `transaction.h` missing `<memory>`: Fix: add `#include <memory>`.

### Phase 3 — BIGINT Support (Task 3) ✅

**Changes made (15 files):**
- `src/defs.h` — Added `TYPE_BIGINT` to `ColType` enum and `coltype2str()`
- `src/parser/ast.h` — Added `SV_TYPE_BIGINT` to `SvType`; changed `IntLit::val` and `SemValue::sv_int` to `int64_t`; added `IntLit::overflow`
- `src/common/common.h` — Added `int64_t bigint_val` to `Value` union; added `set_bigint()`; added `TYPE_BIGINT` to `init_raw()`; added `#include <cstdint>`
- `src/parser/lex.l` — Added `BIGINT` keyword; changed `atoi` → `strtoll` with overflow detection via `thread_local bool rmdb_int_overflow`
- `src/parser/yacc.y` — Added `BIGINT` token and grammar production
- `src/parser/ast_printer.h` — Added `SV_TYPE_BIGINT` mapping
- `src/optimizer/planner.h` — Added `SV_TYPE_BIGINT → TYPE_BIGINT` mapping
- `src/analyze/analyze.cpp` — Added INT↔BIGINT↔FLOAT implicit conversion in SET clause and WHERE clause; fixed `init_raw` ordering bug (moved before type conversion); overflow check in `convert_sv_value`
- `src/execution/executor_seq_scan.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_index_scan.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_nestedloop_join.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_insert.h` — Added INT→BIGINT implicit conversion
- `src/execution/execution_manager.cpp` — Added BIGINT output formatting; updated help text
- `src/index/ix_index_handle.h` — Added `TYPE_BIGINT` to `ix_compare()`
- `src/record_printer.h` — Increased `COL_WIDTH` from 16 to 20 for BIGINT display

**Bugs fixed:**
1. `check_clause` called `init_raw(lhs_col->len)` before type conversion — when BIGINT column (len=8) paired with INT value, triggered `assert(len == sizeof(int))`. Fix: determine `rhs_type` first, convert if needed, then call `init_raw`.
2. `atoi` in lex.l truncated large integer literals to 32-bit. Fix: changed to `strtoll` with `errno`-based overflow detection.

### Phase 3 — DATETIME Support (Task 4) ✅

**Design**: DATETIME stored as `int64_t` packed value `YYYYMMDDHHMMSS` (8 bytes). String conversion via `common/datetime.h` utility header. Validation: year 1000–9999, month 1–12, day validated against month/year (incl. leap year), hour 0–23, minute 0–59, second 0–59. Exactly 19-char format `YYYY-MM-DD HH:MM:SS` enforced.

**Changes made:**
- `src/common/datetime.h` — New file: `parse_datetime()` (parsing + validation), `format_datetime()` (int64_t→string), `is_leap_year()`, `days_in_month()`
- `src/defs.h` — Added `TYPE_DATETIME` to `ColType` and `coltype2str()`
- `src/parser/ast.h` — Added `SV_TYPE_DATETIME` to `SvType`
- `src/common/common.h` — Added `set_datetime()`; added `TYPE_DATETIME` to `init_raw()`; included `datetime.h`
- `src/parser/lex.l` — Added `DATETIME` keyword
- `src/parser/yacc.y` — Added `DATETIME` token and grammar production
- `src/parser/ast_printer.h` — Added `SV_TYPE_DATETIME` mapping
- `src/optimizer/planner.h` — Added `SV_TYPE_DATETIME → TYPE_DATETIME` mapping
- `src/analyze/analyze.cpp` — Added STRING→DATETIME conversion for SET clause and WHERE clause
- `src/execution/executor_insert.h` — Added STRING→DATETIME implicit conversion
- `src/execution/executor_seq_scan.h` — Added `TYPE_DATETIME` to comparison switch (fall-through with `TYPE_BIGINT`)
- `src/execution/executor_index_scan.h` — Same
- `src/execution/executor_nestedloop_join.h` — Same
- `src/execution/execution_manager.cpp` — Added DATETIME output formatting via `format_datetime()`; updated help text
- `src/index/ix_index_handle.h` — Added `TYPE_DATETIME` to `ix_compare()` (fall-through with `TYPE_BIGINT`)

### Phase 2 — Query Execution (Task 2) 🟡

**Changes made (15 files):**
- `src/defs.h` — Added `TYPE_BIGINT` to `ColType` enum and `coltype2str()`
- `src/parser/ast.h` — Added `SV_TYPE_BIGINT` to `SvType`; changed `IntLit::val` and `SemValue::sv_int` to `int64_t`
- `src/common/common.h` — Added `int64_t bigint_val` to `Value` union; added `set_bigint()`; added `TYPE_BIGINT` to `init_raw()`; added `#include <cstdint>`
- `src/parser/lex.l` — Added `BIGINT` keyword; changed `atoi` → `atoll` for integer literals
- `src/parser/yacc.y` — Added `BIGINT` token and grammar production
- `src/parser/ast_printer.h` — Added `SV_TYPE_BIGINT` mapping
- `src/optimizer/planner.h` — Added `SV_TYPE_BIGINT → TYPE_BIGINT` mapping
- `src/analyze/analyze.cpp` — Added INT↔BIGINT↔FLOAT implicit conversion in SET clause and WHERE clause; fixed `init_raw` ordering bug (moved before type conversion)
- `src/execution/executor_seq_scan.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_index_scan.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_nestedloop_join.h` — Added `TYPE_BIGINT` comparison case
- `src/execution/executor_insert.h` — Added INT→BIGINT implicit conversion
- `src/execution/execution_manager.cpp` — Added BIGINT output formatting; updated help text
- `src/index/ix_index_handle.h` — Added `TYPE_BIGINT` to `ix_compare()`
- `src/record_printer.h` — Increased `COL_WIDTH` from 16 to 20 for BIGINT display

**Bugs fixed:**
1. `check_clause` called `init_raw(lhs_col->len)` before type conversion — when BIGINT column (len=8) paired with INT value, triggered `assert(len == sizeof(int))`. Fix: determine `rhs_type` first, convert if needed, then call `init_raw`.
2. `atoi` in lex.l truncated large integer literals to 32-bit. Fix: changed to `atoll` with `int64_t` storage.

### Phase 2 — Query Execution (Task 2) 🟡

**Files modified:**
- `src/system/sm_manager.cpp` — implemented `open_db()`, `close_db()`, `drop_table()`
- `src/execution/executor_seq_scan.h` — implemented beginTuple/nextTuple/Next/is_end/tupleLen/cols + eval_cond/eval_conds helpers
- `src/execution/executor_index_scan.h` — structural implementation (usable once B+ tree methods in Task 5)
- `src/execution/executor_projection.h` — implemented Next(); delegated beginTuple/nextTuple/is_end/tupleLen/cols to prev_
- `src/execution/executor_nestedloop_join.h` — implemented nested-loop join with cached left tuple, right scan restart; added eval_cond/eval_conds helpers
- `src/execution/executor_update.h` — implemented Next() with index maintenance (delete old + insert new entries)
- `src/execution/executor_delete.h` — implemented Next() with index maintenance (delete entries before record)
- `src/analyze/analyze.cpp` — added UpdateStmt handling: SET clause conversion, type checking, WHERE clause
- `src/transaction/transaction_manager.cpp` — minimal begin()/commit()/abort() (prevents nullptr segfault)
- `src/record/CMakeLists.txt` — removed unused `records` shared library
- `src/recovery/CMakeLists.txt` — removed unused `recoverys` shared library
- `src/rmdb.cpp` — Windows/Linux cross-platform fixes (winsock2, DELETE undef, socket_close)
- `src/storage/disk_manager.cpp` — guarded `<io.h>` with `#ifdef _WIN32`; `O_BINARY` → `0` on Linux

**Bugs fixed during Phase 2:**
1. `TransactionManager::begin()` returns nullptr → segfault in SetTransaction. Fix: create Transaction with `next_txn_id_++`, add to txn_map.
2. `ProjectionExecutor` beginTuple/nextTuple/is_end were empty → `select *` returned 0 records. Fix: delegate to prev_.
3. INT/FLOAT type mismatch in WHERE: `score > 90` (FLOAT vs INT). Fix: implicit INT→FLOAT conversion in check_clause.
4. INT/FLOAT type mismatch in UPDATE SET: `score = 0`. Fix: same conversion in UpdateStmt handling.
5. `init_raw()` assertion failure in UpdateExecutor: called twice (analyze + executor). Fix: check `raw == nullptr` first.
6. `<io.h>` breaks Linux build: Fix: `#ifdef _WIN32`.
7. `O_BINARY` not defined on Linux: Fix: `#define O_BINARY 0`.
8. `records`/`recoverys` shared libs -fPIC error on Linux: Fix: removed unused shared library targets.
9. `<readline/*.h>` not available: Fix: removed from rmdb.cpp (server doesn't use it).

### ⚠️ Outstanding issue: Test Point 5 (Join query) segfault

**Symptom**: `select * from t, d;` causes server segfault. Debug logging confirms NestedLoopJoinExecutor's `beginTuple()` is **never called** — crash happens before executor tree is created.

**Likely location**: Planner (`make_one_rel`) or Portal (`convert_plan_executor`) when handling 2-table join without conditions.

**What works**: Single-table SELECT/INSERT/UPDATE/DELETE all correct (SQL test points 1-4 pass on WSL2).

### SQL test results (on WSL2)

| Test Point | Description | Expected | Actual |
|-----------|-------------|----------|--------|
| TP1 | Create/drop tables | `| Tables |` output | ✅ Matches |
| TP2 | Insert + select with WHERE | Filtered records | ✅ Matches |
| TP3 | Update + select | Updated records with INT→FLOAT fix | ✅ Matches |
| TP4 | Delete + select | Empty result after delete | ✅ Matches |
| TP5 | Cross-table join | 6 joined records | ❌ Segfault |

### Cross-platform notes

- The project target is **Ubuntu 18.04+ x86_64**. WSL2 is the recommended dev environment for Windows users.
- `rmdb.cpp` has `#ifdef _WIN32` blocks for winsock2, SHUT_WR, socklen_t, closesocket.
- Build on Windows/MinGW requires `-G "MinGW Makefiles"`.
- Only `unit_test.exe` and `test_parser.exe` build on MinGW. `rmdb.exe` needs `ws2_32`.

### WSL2 migration instructions

Move project from Windows to WSL2 native filesystem (strongly recommended):
```sh
cp -r /mnt/d/AAA/DatabasePractice/rmdb ~/rmdb
cd ~/rmdb
rm -rf build build_wsl
mkdir build && cd build
cmake .. && make -j$(nproc)
./bin/unit_test          # verify Phase 1
./bin/rmdb <db_name> &   # start server for SQL testing
```

**Why migrate**: `/mnt/d/` uses 9P protocol — builds 3-5× slower, cross-filesystem issues cause mysterious bugs. Native ext4 at `~/rmdb/` is the intended environment.

## Coding conventions

- C++17, `-Wall -O0 -g -ggdb3` in debug; release uses `-Wall -O3`
- Copyright header: `/* Copyright (c) 2023 Renmin University of China … Mulan PSL v2 */` at top of every source file
- Class members: `member_name_` (trailing underscore)
- Exceptions: base `RMDBError` in `src/errors.h`; subclasses for each subsystem (e.g. `FileNotFoundError`, `RecordNotFoundError`, `IndexNotFoundError`, `IncompatibleTypeError`, etc.)
- Thread safety: `buffer_mutex` (per-request parser mutex), `sockfd_mutex` (accept mutex), `LockManager::latch_`, `TransactionManager::latch_`, `LogManager::latch_`
- Parser output files (`yacc.tab.{cpp,h}`, `lex.yy.cpp`) are committed — regeneration requires Flex + Bison
- File organization: each subsystem has its own `CMakeLists.txt` producing a static library; public types in `*_defs.h` files
