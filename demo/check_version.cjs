const duckdb = require('./node_modules/@duckdb/duckdb-wasm/dist/duckdb-node-main.cjs');
async function run() {
  const db = new duckdb.AsyncDuckDB(new duckdb.ConsoleLogger(), new duckdb.DuckDBDataProtocol());
  await db.instantiate('./node_modules/@duckdb/duckdb-wasm/dist/duckdb-mvp.wasm');
  const conn = await db.connect();
  const res = await conn.query('SELECT version()');
  console.log("DuckDB Version:", res.toArray().map(r => r.toJSON()));
}
run().catch(console.error);
