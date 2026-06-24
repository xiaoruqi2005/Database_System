#!/usr/bin/env python3
"""Lab5 stress test - covers all lab5.md test cases including edge cases"""
import socket, time, sys, os

HOST, PORT = "127.0.0.1", 8765

def send_sql(sock, sql):
    sql = sql.strip()
    if not sql.endswith(';'): sql += ';'
    sock.sendall(sql.encode('utf-8') + b'\0')
    data = b''
    try:
        while True:
            c = sock.recv(65536)
            if not c: break
            data += c
            if b'\0' in c: break
    except socket.timeout: pass
    return data.decode('utf-8', errors='replace')

def connect():
    for _ in range(30):
        try:
            s = socket.socket(); s.settimeout(15)
            s.connect((HOST, PORT)); return s
        except: time.sleep(0.5)
    return None

def run():
    if os.path.exists('output.txt'): os.remove('output.txt')
    sock = connect()
    if not sock:
        print("FAIL: cannot connect")
        return False

    ok = fail = 0
    def t(sql, expect_fail=False):
        nonlocal ok, fail
        try:
            r = send_sql(sock, sql)
            f = 'failure' in r.lower()
            if f == expect_fail:
                print(f"  OK: {sql[:80]}")
                ok += 1
            else:
                print(f"  FAIL: {sql[:80]} (exp={'fail' if expect_fail else 'ok'}, got={'fail' if f else 'ok'})")
                fail += 1
        except Exception as e:
            print(f"  ERR: {sql[:80]} -> {e}")
            fail += 1
        time.sleep(0.05)

    # === Part 1: Large-scale index create/query/drop ===
    print("--- Part 1: Large-scale index operations ---")
    send_sql(sock, "create table big (id int, val int, name char(16))")
    for i in range(1, 51):
        send_sql(sock, f"insert into big values ({i}, {i*10}, 'row{i:04d}')")
    t("create index big(id)")
    t("create index big(val)")
    t("create index big(id,val)")
    t("show index from big")
    # Range queries
    t("select * from big where id = 25")
    t("select * from big where id > 40 and id < 46")
    t("select * from big where val = 100")
    t("select * from big where val > 300 and val < 400")
    t("select * from big where id = 10 and val = 100")
    # Updates with index
    t("update big set val = 999 where id = 1")
    t("update big set id = 99 where id = 50")
    t("select * from big where val = 999")
    t("select * from big where id = 99")
    # Deletes with index
    t("delete from big where id = 2")
    t("delete from big where id = 3")
    t("select * from big where id = 2")
    t("select * from big where id = 3")
    # Drop some indexes and query
    t("drop index big(id)")
    t("drop index big(id,val)")
    t("show index from big")
    t("select * from big where val > 400")
    # Drop table (should cascade drop idx)
    send_sql(sock, "drop table big")

    # === Part 2: lab5.md exact test cases ===
    print("--- Part 2: lab5.md Part 1 (Create/Drop/Show Index) ---")
    send_sql(sock, "create table warehouse (id int, name char(8))")
    t("create index warehouse (id)")
    t("show index from warehouse")
    t("create index warehouse (id,name)")
    t("show index from warehouse")
    t("drop index warehouse (id)")
    t("drop index warehouse (id,name)")
    t("show index from warehouse")
    send_sql(sock, "drop table warehouse")

    print("--- Part 3: lab5.md Part 2 (Index Query) ---")
    send_sql(sock, "create table warehouse2 (w_id int, name char(8))")
    send_sql(sock, "insert into warehouse2 values (10 , 'qweruiop')")
    send_sql(sock, "insert into warehouse2 values (534, 'asdfhjkl')")
    send_sql(sock, "insert into warehouse2 values (100,'qwerghjk')")
    send_sql(sock, "insert into warehouse2 values (500,'bgtyhnmj')")
    t("create index warehouse2(w_id)")
    t("select * from warehouse2 where w_id = 10")
    t("select * from warehouse2 where w_id < 534 and w_id > 100")
    t("drop index warehouse2(w_id)")
    t("create index warehouse2(name)")
    t("select * from warehouse2 where name = 'qweruiop'")
    t("select * from warehouse2 where name > 'qwerghjk'")
    t("select * from warehouse2 where name > 'aszdefgh' and name < 'qweraaaa'")
    t("drop index warehouse2(name)")
    t("create index warehouse2(w_id,name)")
    t("select * from warehouse2 where w_id = 100 and name = 'qwerghjk'")
    t("select * from warehouse2 where w_id < 600 and name > 'bztyhnmj'")
    send_sql(sock, "drop table warehouse2")

    print("--- Part 4: lab5.md Part 3 (Index Maintenance) ---")
    send_sql(sock, "create table warehouse3 (w_id int, name char(8))")
    send_sql(sock, "insert into warehouse3 values (10 , 'qweruiop')")
    send_sql(sock, "insert into warehouse3 values (534, 'asdfhjkl')")
    t("select * from warehouse3 where w_id = 10")
    t("select * from warehouse3 where w_id < 534 and w_id > 100")
    t("create index warehouse3(w_id)")
    t("insert into warehouse3 values (500, 'lastdanc')")
    t("update warehouse3 set w_id = 507 where w_id = 534")
    t("select * from warehouse3 where w_id = 10")
    t("select * from warehouse3 where w_id < 534 and w_id > 100")
    send_sql(sock, "drop table warehouse3")

    # === Part 5: Edge cases - create/drop database ===
    print("--- Part 5: Edge cases ---")
    send_sql(sock, "create table edge (a int, b int, c char(4))")
    for i in range(1, 201):
        send_sql(sock, f"insert into edge values ({i}, {i%10}, 's{i%26+65}')")
    t("create index edge(a)")
    t("create index edge(b)")
    t("select * from edge where a = 100")
    t("select * from edge where a > 50 and a < 60")
    t("select * from edge where b = 5")
    # Many updates
    for i in range(10):
        send_sql(sock, f"update edge set b = {i+10} where a = {i+1}")
    t("select * from edge where b = 10")
    # Delete some
    for i in range(5):
        send_sql(sock, f"delete from edge where a = {i+1}")
    t("select * from edge where a < 10")
    t("drop index edge(a)")
    t("drop index edge(b)")
    send_sql(sock, "drop table edge")

    sock.close()
    time.sleep(0.5)
    if os.path.exists('output.txt'):
        with open('output.txt') as f:
            lines = f.readlines()
        print(f"\n=== output.txt ({len(lines)} lines) ===")
    print(f"\nResults: {ok} pass, {fail} fail")
    return fail == 0

if __name__ == '__main__':
    sys.exit(0 if run() else 1)