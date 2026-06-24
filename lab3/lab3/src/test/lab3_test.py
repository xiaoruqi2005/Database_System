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
                print(f'Cannot connect to {host}:{port}: {e}')
                return False
            time.sleep(1)
    print(f'Connected to RMDB server {host}:{port}')
    time.sleep(0.5)

    # Remove old output file
    if os.path.exists(output_path):
        os.remove(output_path)

    sqls = [
        'CREATE TABLE t(bid bigint,sid int)',
        'INSERT INTO t VALUES(372036854775807,233421)',
        'INSERT INTO t VALUES(-922337203685477580,124332)',
        'SELECT * FROM t',
        'INSERT INTO t VALUES(9223372036854775809,12345)',
        'SELECT * FROM t',
    ]
    for sql in sqls:
        resp = send_sql(sock, sql)
        print(f'SQL: {sql} -> response len={len(resp)}')
        time.sleep(0.3)

    time.sleep(1)
    if not os.path.exists(output_path):
        print('ERROR: output.txt not created!')
        sock.close()
        return False

    with open(output_path, 'r') as f:
        content = f.read()

    print('=== output.txt content ===')
    print(content)
    print('=== end ===')

    lines = [l.strip() for l in content.splitlines() if l.strip()]
    expected = [
        '| bid | sid |',
        '| 372036854775807 | 233421 |',
        '| -922337203685477580 | 124332 |',
        '| bid | sid |',
        '| 372036854775807 | 233421 |',
        '| -922337203685477580 | 124332 |',
    ]

    has_failure = 'failure' in content.lower()
    if has_failure:
        print('[CHECK] failure detected for overflow: OK')
    else:
        print('[CHECK] failure NOT detected for overflow: FAIL')

    ok = (lines == expected) and has_failure
    if ok:
        print('[RESULT] lab3.md example: PASS')
    else:
        print('[RESULT] lab3.md example: FAIL')
        print(f'  Expected: {expected}')
        print(f'  Got:      {lines}')

    sock.close()
    return ok

if __name__ == '__main__':
    output_path = sys.argv[1] if len(sys.argv) > 1 else 'output.txt'
    success = run_tests('127.0.0.1', 8765, output_path)
    sys.exit(0 if success else 1)
