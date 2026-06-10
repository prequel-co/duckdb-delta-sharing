import duckdb
import threading
import time

con = duckdb.connect()
con.execute("LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension'")
con.execute("LOAD httpfs")
con.execute("CREATE SECRET (TYPE delta_sharing, PROVIDER env)")

def run_info_schema():
    for _ in range(50):
        con.execute("SELECT * FROM information_schema.columns").fetchall()

def run_read():
    for _ in range(5):
        con.execute("SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders') LIMIT 1").fetchall()

t1 = threading.Thread(target=run_info_schema)
t2 = threading.Thread(target=run_read)

t1.start()
t2.start()

t1.join()
t2.join()
print("Done")
