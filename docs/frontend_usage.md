# Using DuckDB WASM with Delta Sharing in the Browser

This guide explains how to integrate the DuckDB Delta Sharing extension into a modern front-end web application (React, Vue, vanilla TS + Vite, etc.). It is based on the included `demo` application.

## Prerequisites

You need the DuckDB WASM library installed in your project. If you are using `npm` or `pnpm`:

```bash
npm install @duckdb/duckdb-wasm
```

*Note: Since the Delta Sharing extension is built for the very latest DuckDB versions, you will likely need to use the `1.33.1-dev57.0` (or newer) release of `@duckdb/duckdb-wasm` to match DuckDB v1.5.5+.*

## 1. Initialization and Setup

DuckDB WASM relies on Web Workers to run in the background. You must first resolve the location of these WebAssembly and Worker files. 

If you are using Vite, you can import them directly as URLs:

```typescript
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
```

Next, instantiate the database. **Crucially**, you must open the database with `allowUnsignedExtensions: true` because the Delta Sharing extension is currently unsigned.

```typescript
let db: duckdb.AsyncDuckDB | null = null;
let conn: duckdb.AsyncDuckDBConnection | null = null;

export async function initDuckDB() {
    // Select the best bundle for the user's browser
    const bundle = await duckdb.selectBundle(MANUAL_BUNDLES);
    
    const worker = new Worker(bundle.mainWorker!);
    const logger = new duckdb.ConsoleLogger();
    
    db = new duckdb.AsyncDuckDB(logger, worker);
    await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
    
    // Allow unsigned extensions
    await db.open({ allowUnsignedExtensions: true });
    
    conn = await db.connect();
    return db;
}
```

## 2. Loading the Delta Sharing Extension

The Delta Sharing extension depends on DuckDB's official `parquet` and `json` extensions. You must `LOAD` those first. 

Since DuckDB WASM loads extensions over HTTP, you need to tell it where your compiled extension lives by setting the `custom_extension_repository` path. If the extension WASM files are hosted at the root of your domain, you can just use `window.location.origin`.

```typescript
export async function loadDeltaSharingExtension() {
    if (!db || !conn) throw new Error("DuckDB not initialized");
    
    // 1. Load dependencies
    await conn.query(`LOAD parquet;`);
    await conn.query(`LOAD json;`);
    
    // 2. Set the repository path where your extension WASM files are hosted
    const origin = window.location.origin;
    await conn.query(`SET custom_extension_repository='${origin}';`);
    
    // 3. Load the extension
    await conn.query(`LOAD duckdb_delta_sharing;`);
}
```

## 3. Authenticating / Configuring a Share

To access a Delta Share, you use DuckDB's Secret Manager. You will create a secret of type `delta_sharing` using the endpoint URL and the bearer token.

Typically, this data is loaded from a `.share` profile file (which is just JSON), or manually entered by the user.

```typescript
export async function setupDeltaSharingSecret(endpoint: string, bearerToken: string) {
    if (!conn) throw new Error("DuckDB not connected");
    
    // Clean up any old secret
    await conn.query(`DROP SECRET IF EXISTS my_delta_share_secret;`);
    
    // Create the new secret
    await conn.query(`
        CREATE SECRET my_delta_share_secret (
            TYPE delta_sharing,
            PROVIDER config,
            ENDPOINT '${endpoint}',
            BEARER_TOKEN '${bearerToken}'
        );
    `);
}
```

## 4. Querying and Rendering Data

You can now run standard SQL queries against Delta Shares! For example:
`SELECT * FROM delta_scan('my_share', 'my_schema', 'my_table')`

### Important Note on UI Data Serialization

DuckDB WASM returns data using Apache Arrow formats. When converting these to plain JavaScript objects for UI rendering, be aware of two specific data types that can crash reactive UI frameworks (like ArrowJS, Vue, or React) if left unchecked:

1. **`BigInt`**: Returned for large integers. Can't be serialized to standard JSON without throwing `TypeError: Do not know how to serialize a BigInt`.
2. **`TypedArray` (e.g., `Uint32Array`)**: Used internally to represent DuckDB `DECIMAL` types.

To prevent your UI from crashing, it's highly recommended to sanitize the rows returned from your queries before placing them into your reactive application state.

```typescript
export async function runQuery(sql: string): Promise<any[]> {
    if (!conn) throw new Error("DuckDB not connected");
    
    const result = await conn.query(sql);
    
    // Convert Arrow table to an array of objects
    const rows = result.toArray().map(row => row.toJSON());
    
    // Sanitize results for UI state
    return rows.map(row => {
        const sanitized: any = {};
        
        for (const [key, value] of Object.entries(row)) {
            if (value === null || value === undefined) {
                sanitized[key] = null;
            } else if (typeof value === 'bigint') {
                // Convert BigInts to strings
                sanitized[key] = value.toString();
            } else if (ArrayBuffer.isView(value)) {
                // Handle TypedArrays (like DuckDB decimals)
                sanitized[key] = `[${value.constructor.name}]`;
            } else if (typeof value === 'object') {
                // Handle nested objects safely
                sanitized[key] = JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v);
            } else {
                sanitized[key] = value;
            }
        }
        
        return sanitized;
    });
}
```

Now you can bind the sanitized data directly to your frontend framework's UI state.
