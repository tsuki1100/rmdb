#!/usr/bin/env python3
"""SQL test harness for RMDB — connects to server over TCP, sends SQL, prints results."""
import socket
import subprocess
import sys
import os
import time
import signal
import shutil

HOST = "127.0.0.1"
PORT = 8765
BUFFER_SIZE = 8192


def sql(sql_str):
    """Send one SQL, return result text. Each call opens/closes its own connection."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, PORT))
    s.sendall((sql_str + '\0').encode())
    data = s.recv(BUFFER_SIZE)
    s.close()
    return data.decode().rstrip('\0 \n\r')


def run_test(name, sql_cmds, check_fn=None):
    """Run a test: execute list of SQL commands, optionally check results."""
    print(f"\n{'='*60}")
    print(f"Running {name}...")
    print(f"{'='*60}")
    try:
        for cmd in sql_cmds:
            r = sql(cmd)
            if r and r not in ('', 'failure'):
                print(f"  {cmd[:50]} -> ok")
            elif r == 'failure':
                print(f"  {cmd[:50]} -> FAILURE: {r}")
            # Skip empty responses (success with no data)
        if check_fn:
            check_fn()
        print(f">>> {name}: ✅ PASS")
        return True
    except Exception as e:
        print(f">>> {name}: ❌ FAIL — {e}")
        import traceback
        traceback.print_exc()
        return False


def verify_output(cmd, expected_contains):
    """Verify a SQL command's output contains expected strings."""
    result = sql(cmd)
    for exp in expected_contains:
        assert exp in result, f"Expected '{exp}' in result, got:\n{result}"
    return result


def test_tp1():
    """TP1: DDL — create/drop tables, show tables."""
    cmds = [
        "create table t(id int, name char(10), score float);",
        "create table d(id int, dept char(10));",
    ]
    for c in cmds:
        r = sql(c)
        assert r != 'failure', f"DDL failed: {c}"
    result = sql("show tables;")
    assert "t" in result and "d" in result, f"Expected t and d. Got: {result}"
    sql("drop table t;")
    sql("drop table d;")
    print(f"  show tables: {repr(result)}")


def test_tp2():
    """TP2: INSERT + SELECT with WHERE."""
    sql("create table t(id int, name char(10), score float);")
    sql("insert into t values(1, 'Alice', 95.5);")
    sql("insert into t values(2, 'Bob', 88.0);")
    sql("insert into t values(3, 'Charlie', 72.5);")
    r = verify_output("select * from t where score > 80;",
                      ["Alice", "Bob"])
    assert "Charlie" not in r
    sql("drop table t;")
    print(f"  result: {r[:80]}...")


def test_tp3():
    """TP3: UPDATE + SELECT."""
    sql("create table t(id int, name char(10), score float);")
    sql("insert into t values(1, 'Alice', 95.5);")
    sql("insert into t values(2, 'Bob', 88.0);")
    sql("update t set score = 0 where id = 1;")
    r = verify_output("select * from t where score = 0;", ["Alice"])
    sql("drop table t;")
    print(f"  result: {r[:80]}...")


def test_tp4():
    """TP4: DELETE + SELECT."""
    sql("create table t(id int, name char(10), score float);")
    sql("insert into t values(1, 'Alice', 95.5);")
    sql("insert into t values(2, 'Bob', 88.0);")
    sql("delete from t where id = 1;")
    r = sql("select * from t;")
    assert "Alice" not in r, f"Expected Alice deleted. Got: {r}"
    assert "Bob" in r
    sql("drop table t;")
    print(f"  result: {r[:80]}...")


def test_tp5():
    """TP5: Cross-table JOIN (two tables, no condition)."""
    sql("create table t(id int, name char(10), score float);")
    sql("create table d(id int, dept char(10));")
    sql("insert into t values(1, 'Alice', 95.5);")
    sql("insert into t values(2, 'Bob', 88.0);")
    sql("insert into d values(1, 'CS');")
    sql("insert into d values(2, 'Math');")

    r = sql("select * from t, d;")
    assert all(x in r for x in ["Alice", "Bob", "CS", "Math"]), f"Join returned wrong data:\n{r}"
    assert "Total record(s): 4" in r, f"Expected 4 records:\n{r}"
    print(f"  Cross join (2x2=4 records): ✅")

    r = sql("select t.id, d.dept from t, d where t.id = d.id;")
    assert all(x in r for x in ["CS", "Math"]), f"Join with WHERE failed:\n{r}"
    assert "Total record(s): 2" in r
    print(f"  Join with WHERE (2 records): ✅")

    sql("drop table t;")
    sql("drop table d;")


def main():
    # Parse args
    db_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rmdb_test_all"
    bin_path = sys.argv[2] if len(sys.argv) > 2 else "./build/bin/rmdb"

    # Clean db dir — server will recreate with db.meta
    shutil.rmtree(db_path, ignore_errors=True)

    print(f"Starting server: {bin_path} {db_path}")
    proc = subprocess.Popen(
        [bin_path, db_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        preexec_fn=os.setsid,
    )

    # Wait for server to be ready
    start = time.time()
    while time.time() - start < 10:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect((HOST, PORT))
            s.close()
            time.sleep(0.3)
            break
        except (ConnectionRefusedError, OSError):
            if proc.poll() is not None:
                raise RuntimeError(f"Server died (exit {proc.poll()})")
            time.sleep(0.3)
    else:
        raise RuntimeError("Server did not start")

    print("Server is ready.\n")

    tests = [
        ("TP1 — DDL Create/Drop", lambda: test_tp1()),
        ("TP2 — INSERT + SELECT WHERE", lambda: test_tp2()),
        ("TP3 — UPDATE + SELECT", lambda: test_tp3()),
        ("TP4 — DELETE + SELECT", lambda: test_tp4()),
        ("TP5 — Cross-table JOIN", lambda: test_tp5()),
    ]

    passed = 0
    failed = 0
    for name, fn in tests:
        if run_test(name, [], fn):
            passed += 1
        else:
            failed += 1

    print(f"\n{'='*60}")
    print(f"Results: {passed}/{len(tests)} passed, {failed}/{len(tests)} failed")

    # Stop server
    os.killpg(os.getpgid(proc.pid), signal.SIGINT)
    proc.wait(timeout=5)

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
