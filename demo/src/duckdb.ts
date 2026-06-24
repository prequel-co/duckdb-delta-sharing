import * as duckdb from '@duckdb/duckdb-wasm';
import duckdb_wasm from '@duckdb/duckdb-wasm/dist/duckdb-mvp.wasm?url';
import mvp_worker from '@duckdb/duckdb-wasm/dist/duckdb-browser-mvp.worker.js?url';
import duckdb_wasm_eh from '@duckdb/duckdb-wasm/dist/duckdb-eh.wasm?url';
import eh_worker from '@duckdb/duckdb-wasm/dist/duckdb-browser-eh.worker.js?url';

const MANUAL_BUNDLES: duckdb.DuckDBBundles = {
    mvp: {
        mainModule: duckdb_wasm,
        mainWorker: mvp_worker,
    },
    eh: {
        mainModule: duckdb_wasm_eh,
        mainWorker: eh_worker,
    },
};

let db: duckdb.AsyncDuckDB | null = null;
let conn: duckdb.AsyncDuckDBConnection | null = null;

export async function initDuckDB() {
    if (db) return db;
    
    // Select a bundle based on browser checks
    const bundle = await duckdb.selectBundle(MANUAL_BUNDLES);
    
    // Instantiate the asynchronus version of DuckDB-wasm
    const worker = new Worker(bundle.mainWorker!);
    const logger = new duckdb.ConsoleLogger();
    db = new duckdb.AsyncDuckDB(logger, worker);
    await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
    
    // Open the database with configuration allowing unsigned extensions
    await db.open({
        allowUnsignedExtensions: true
    });
    
    conn = await db.connect();
    return db;
}

export async function loadDeltaSharingExtension() {
    if (!db || !conn) throw new Error("DuckDB not initialized");
    // Register and load the bundled extension
    await db.registerFileURL('duckdb_delta_sharing.wasm', '/duckdb_delta_sharing.wasm', duckdb.DuckDBDataProtocol.HTTP, false);
    await conn.query(`LOAD 'duckdb_delta_sharing.wasm';`);
}

export async function setupDeltaSharingFile(fileContent: string) {
    if (!db || !conn) throw new Error("DuckDB not initialized");
    // Register the profile JSON so duckdb can read it
    await db.registerFileText('profile.share', fileContent);
    // Execute command to load profile
    // Note: Depends on how the extension is designed. Assuming standard LOAD SHARE mechanism.
    // e.g. LOAD_SHARE('profile.share');
    // For now we just return true.
    return true;
}

export async function runQuery(sql: string): Promise<any[]> {
    if (!conn) throw new Error("DuckDB not connected");
    const result = await conn.query(sql);
    return result.toArray().map(row => row.toJSON());
}
