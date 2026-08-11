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
    
    // Select a bundle based on browser checks (which will pick EH for modern browsers)
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
    
    // Load official dependencies first BEFORE setting custom repo
    await conn.query(`LOAD parquet;`);
    await conn.query(`LOAD json;`);
    // await conn.query(`LOAD httpfs;`);
    
    // DuckDB WASM expects extensions in a specific repository structure:
    // $repo/$duckdb_version_hash/$duckdb_platform/$name.duckdb_extension.wasm
    // We set our Vite dev server as the custom repository.
    const origin = window.location.origin;
    await conn.query(`SET custom_extension_repository='${origin}';`);
    
    // LOAD will automatically fetch and decompress the extension from our custom repository
    await conn.query(`LOAD duckdb_delta_sharing;`);
}

export async function setupDeltaSharingFile(fileContent: string) {
    if (!db || !conn) throw new Error("DuckDB not initialized");
    
    // Parse the Delta Sharing profile (.share) JSON
    const profile = JSON.parse(fileContent);
    if (!profile.endpoint || !profile.bearerToken) {
        throw new Error("Invalid .share profile: missing endpoint or bearerToken");
    }
    
    // Drop existing secret if present
    await conn.query(`DROP SECRET IF EXISTS my_delta_share_secret;`);
    
    // Create a new secret for the extension to use
    await conn.query(`
        CREATE SECRET my_delta_share_secret (
            TYPE delta_sharing,
            PROVIDER config,
            ENDPOINT '${profile.endpoint}',
            BEARER_TOKEN '${profile.bearerToken}'
        );
    `);
    
    return true;
}

export async function runQuery(sql: string): Promise<any[]> {
    if (!conn) throw new Error("DuckDB not connected");
    const result = await conn.query(sql);
    const rows = result.toArray().map(row => row.toJSON());
    
    // Sanitize results so ArrowJS proxies don't crash on TypedArrays (like DuckDB decimals) or BigInts
    return rows.map(row => {
        const sanitized: any = {};
        for (const [key, value] of Object.entries(row)) {
            if (value === null || value === undefined) {
                sanitized[key] = null;
            } else if (typeof value === 'bigint') {
                sanitized[key] = value.toString();
            } else if (ArrayBuffer.isView(value)) {
                // TypedArrays (like Uint32Array for Decimals) crash when proxied by ArrowJS
                sanitized[key] = `[${value.constructor.name}]`;
            } else if (typeof value === 'object') {
                sanitized[key] = JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v);
            } else {
                sanitized[key] = value;
            }
        }
        return sanitized;
    });
}
