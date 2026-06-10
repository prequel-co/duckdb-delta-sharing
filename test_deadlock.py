import duckdb
import os

print("Connecting...")
con = duckdb.connect()

print("Loading extension...")
con.execute("LOAD './build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension'")
con.execute("LOAD httpfs")
con.execute(f"CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '{os.environ.get('DELTA_SHARING_ENDPOINT', 'https://eastus-c3.azuredatabricks.net/api/2.0/delta-sharing/metastores/88b565d0-d549-486d-8854-fad58d9a179c')}', BEARER_TOKEN '{os.environ.get('DELTA_SHARING_BEARER_TOKEN', '')}')")

SHARE = "prequel_dev_share"
SCHEMA = "prequel_dev"
TABLE = "orders"

print("Creating view...")
con.execute(f"CREATE VIEW my_orders_view AS SELECT * FROM delta_share_read('{SHARE}', '{SCHEMA}', '{TABLE}')")

print("Querying duckdb_columns()...")
try:
    con.execute("SELECT * FROM duckdb_columns()").fetchall()
    print("duckdb_columns OK")
except Exception as e:
    print("duckdb_columns error:", e)

print("Querying information_schema.columns...")
try:
    con.execute("SELECT * FROM information_schema.columns").fetchall()
    print("information_schema.columns OK")
except Exception as e:
    print("information_schema.columns error:", e)

print("Querying information_schema.tables...")
try:
    con.execute("SELECT * FROM information_schema.tables").fetchall()
    print("information_schema.tables OK")
except Exception as e:
    print("information_schema.tables error:", e)

print("Done")
