#!/usr/bin/env python3
import socket, time, sys, os

def send_sql(sock, sql):
    sql = sql.strip()
    if not sql.endswith(';'):
        sql = sql + ';'
    sock.sendall(sql.encode('utf-8') + b'\0')
    data = b''
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
            if b'\0' in chunk:
                break
    except socket.timeout:
        pass
    return data

def run_tests(host, port, output_path):
    sock = None
    for attempt in range(15):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((host, port))
            break
        except (ConnectionRefusedError, socket.timeout) as e:
            if sock: sock.close()
            if attempt == 14:
                print(f'Cannot connect: {e}')
                return False
            time.sleep(1)
    print(f'Connected')
    time.sleep(0.5)

    if os.path.exists(output_path):
        os.remove(output_path)

    print('=== TEST POINT 1 ===')
    sqls1 = [
        'CREATE TABLE t(id int , time datetime)',
        "INSERT INTO t values(1, '2023-05-18 09:12:19')",
        "INSERT INTO t values(2, '2023-05-30 12:34:32')",
        'SELECT * FROM t',
        "DELETE FROM t WHERE time = '2023-05-30 12:34:32'",
        "UPDATE t SET id = 2023 WHERE time = '2023-05-18 09:12:19'",
        'SELECT * FROM t',
    ]
    for sql in sqls1:
        resp = send_sql(sock, sql)
        resp_str = resp.decode('utf-8', errors='replace')
        is_failure = 'failure' in resp_str.lower()
        print(f'  SQL: {sql[:60]} -> {"failure" if is_failure else "ok"}')
        time.sleep(0.3)

    time.sleep(0.5)
    if os.path.exists(output_path):
        with open(output_path) as f:
            content1 = f.read()
        lines1 = [l.strip() for l in content1.splitlines() if l.strip()]
        expected1 = [
            '| id | time |',
            '| 1 | 2023-05-18 09:12:19 |',
            '| 2 | 2023-05-30 12:34:32 |',
            '| id | time |',
            '| 2023 | 2023-05-18 09:12:19 |',
        ]
        if lines1 == expected1:
            print('  [TEST1] PASS')
        else:
            print('  [TEST1] FAIL')
            print(f'    Got: {lines1}')
    else:
        print('  ERROR: no output.txt')

    print('=== TEST POINT 2 ===')
    send_sql(sock, 'DROP TABLE t')
    time.sleep(0.3)

    if os.path.exists(output_path):
        os.remove(output_path)

    sqls2 = [
        'CREATE TABLE t(time datetime, temperature float)',
        "INSERT INTO t values('1999-07-07 12:30:00' , 36.0)",
        'SELECT * FROM t',
        "INSERT INTO t values('1999-13-07 12:30:00' , 36.0)",
        "INSERT INTO t values('1999-1-07 12:30:00' , 36.0)",
        "INSERT INTO t values('1999-00-07 12:30:00' , 36.0)",
        "INSERT INTO t values('1999-07-00 12:30:00' , 36.0)",
        "INSERT INTO t values('0001-07-10 12:30:00' , 36.0)",
        "INSERT INTO t values('1999-02-30 12:30:00' , 36.0)",
        "INSERT INTO t values('1999-02-28 12:30:61' , 36.0)",
        'SELECT * FROM t',
    ]
    for i, sql in enumerate(sqls2):
        resp = send_sql(sock, sql)
        resp_str = resp.decode('utf-8', errors='replace')
        is_failure = 'failure' in resp_str.lower()
        should_fail = (3 <= i <= 9)
        match = 'OK' if (is_failure == should_fail) else 'MISMATCH'
        print(f'  SQL[{i}]: {sql[:50]} -> {"fail" if is_failure else "ok"} (exp {"fail" if should_fail else "ok"}) {match}')
        time.sleep(0.3)

    time.sleep(1)
    if os.path.exists(output_path):
        with open(output_path) as f:
            content2 = f.read()
        print(f'=== Test2 output ===')
        print(content2)
        lines2 = [l.strip() for l in content2.splitlines() if l.strip()]
        expected2 = [
            '| time | temperature |',
            '| 1999-07-07 12:30:00 | 36.000000 |',
            '| time | temperature |',
            '| 1999-07-07 12:30:00 | 36.000000 |',
        ]
        if lines2 == expected2:
            print('  [TEST2] PASS')
        else:
            print('  [TEST2] FAIL')
            print(f'    Got: {lines2}')
    else:
        print('  ERROR: no output.txt')

    sock.close()

if __name__ == '__main__':
    output_path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/lab4_test_db/output.txt'
    run_tests('127.0.0.1', 8765, output_path)