#!/usr/bin/env python3
"""Lab5 B+Tree Index Test"""
import socket,time,sys,os

HOST="127.0.0.1"; PORT=8765

def send_sql(sock, sql):
    sql=sql.strip()
    if not sql.endswith(';'): sql=sql+';'
    sock.sendall(sql.encode('utf-8')+b'\0')
    data=b''
    try:
        while True:
            c=sock.recv(65536)
            if not c: break
            data+=c
            if b'\0' in c: break
    except socket.timeout: pass
    return data.decode('utf-8',errors='replace')

def connect():
    for _ in range(20):
        try:
            s=socket.socket();s.settimeout(10)
            s.connect((HOST,PORT)); return s
        except: time.sleep(0.5)
    return None

def run():
    if os.path.exists('output.txt'): os.remove('output.txt')
    sock=connect()
    if not sock: print("FAIL: connect"); return False
    ok=fail=0
    def t(sql, expect_fail=False):
        nonlocal ok,fail
        try:
            r=send_sql(sock,sql)
            f='failure' in r.lower()
            if f==expect_fail: print(f"OK: {sql[:60]}"); ok+=1
            else: print(f"FAIL: {sql[:60]} (exp={'fail' if expect_fail else 'ok'} got={'fail' if f else 'ok'})"); fail+=1
        except Exception as e: print(f"ERR: {sql[:60]} -> {e}"); fail+=1

    print("=== Part1: Create/Drop/Show Index ===")
    send_sql(sock,"create table w(id int,name char(8))")
    t("create index w(id)")
    t("show index from w")
    t("create index w(id,name)")
    t("show index from w")
    t("drop index w(id)")
    t("drop index w(id,name)")
    t("show index from w")
    send_sql(sock,"drop table w")

    print("\n=== Part2: Index Query ===")
    send_sql(sock,"create table w2(w_id int,name char(8))")
    send_sql(sock,"insert into w2 values(10,'qweruiop')")
    send_sql(sock,"insert into w2 values(534,'asdfhjkl')")
    send_sql(sock,"insert into w2 values(100,'qwerghjk')")
    send_sql(sock,"insert into w2 values(500,'bgtyhnmj')")
    t("create index w2(w_id)")
    t("select * from w2 where w_id=10")
    t("select * from w2 where w_id<534 and w_id>100")
    t("drop index w2(w_id)")
    t("create index w2(name)")
    t("select * from w2 where name='qweruiop'")
    t("select * from w2 where name>'qwerghjk'")
    t("select * from w2 where name>'aszdefgh' and name<'qweraaaa'")
    t("drop index w2(name)")
    t("create index w2(w_id,name)")
    t("select * from w2 where w_id=100 and name='qwerghjk'")
    t("select * from w2 where w_id<600 and name>'bztyhnmj'")
    send_sql(sock,"drop table w2")

    print("\n=== Part3: Index Maintenance ===")
    send_sql(sock,"create table w3(w_id int,name char(8))")
    send_sql(sock,"insert into w3 values(10,'qweruiop')")
    send_sql(sock,"insert into w3 values(534,'asdfhjkl')")
    t("select * from w3 where w_id=10")
    t("select * from w3 where w_id<534 and w_id>100")
    t("create index w3(w_id)")
    t("insert into w3 values(500,'lastdanc')")
    t("update w3 set w_id=507 where w_id=534")
    t("select * from w3 where w_id=10")
    t("select * from w3 where w_id<534 and w_id>100")
    send_sql(sock,"drop table w3")
    sock.close()

    time.sleep(0.5)
    if os.path.exists('output.txt'):
        with open('output.txt') as f:
            print(f"\n=== output.txt ({len(f.readlines())} lines) ===")
    print(f"\nResults: {ok} pass, {fail} fail")
    return fail==0

if __name__=='__main__':
    sys.exit(0 if run() else 1)