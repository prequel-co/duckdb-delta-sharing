import * as duckdb from '@duckdb/duckdb-wasm';
import * as worker_js from '@duckdb/duckdb-wasm/dist/duckdb-node-eh.worker.cjs';
import fs from 'fs';

async function run() {
    const bundle = await duckdb.selectBundle({
        mvp: {
            mainModule: 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-mvp.wasm',
            mainWorker: 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-node-mvp.worker.cjs'
        },
        eh: {
            mainModule: 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm',
            mainWorker: 'node_modules/@duckdb/duckdb-wasm/dist/duckdb-node-eh.worker.cjs'
        }
    });
    
    const logger = new duckdb.ConsoleLogger();
    const WorkerClass = worker_js.default ? worker_js.default : worker_js;
    const worker = new WorkerClass();
    const db = new duckdb.AsyncDuckDB(logger, worker);
    await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
    
    await db.open({ allowUnsignedExtensions: true });
    const conn = await db.connect();
    
    let res = await conn.query("SELECT version(), duckdb_version();");
    console.log(res.toArray().map(r => r.toJSON()));
    
    await conn.close();
    await db.terminate();
}

run().catch(console.error);
