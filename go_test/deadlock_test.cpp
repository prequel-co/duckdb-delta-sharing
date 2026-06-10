#include "duckdb.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

duckdb_database db;
duckdb_connection con1, con2;

void* run_info_schema(void* arg) {
    for (int i = 0; i < 500; i++) {
        duckdb_result result;
        if (duckdb_query(con1, "SELECT * FROM information_schema.columns", &result) == DuckDBError) {
            printf("Error in info schema: %s\n", duckdb_result_error(&result));
            duckdb_destroy_result(&result);
            return NULL;
        }
        duckdb_destroy_result(&result);
    }
    printf("Info schema completed\n");
    return NULL;
}

void* run_read(void* arg) {
    for (int i = 0; i < 50; i++) {
        duckdb_result result;
        if (duckdb_query(con2, "SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders') LIMIT 1", &result) == DuckDBError) {
            printf("Error in read: %s\n", duckdb_result_error(&result));
            duckdb_destroy_result(&result);
            return NULL;
        }
        duckdb_destroy_result(&result);
    }
    printf("Read completed\n");
    return NULL;
}

int main() {
    char* endpoint = getenv("DELTA_SHARING_ENDPOINT");
    char* token = getenv("DELTA_SHARING_BEARER_TOKEN");
    
    duckdb_config config;
    duckdb_create_config(&config);
    duckdb_set_config(config, "allow_unsigned_extensions", "true");
    
    if (duckdb_open_ext(NULL, &db, config, NULL) == DuckDBError) return 1;
    duckdb_destroy_config(&config);
    
    if (duckdb_connect(db, &con1) == DuckDBError) return 1;
    if (duckdb_connect(db, &con2) == DuckDBError) return 1;
    
    duckdb_result res;
    duckdb_query(con1, "LOAD '../build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension'", &res); duckdb_destroy_result(&res);
    duckdb_query(con1, "LOAD httpfs", &res); duckdb_destroy_result(&res);
    
    char q[1024];
    snprintf(q, sizeof(q), "CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '%s', BEARER_TOKEN '%s')", endpoint, token);
    duckdb_query(con1, q, &res); duckdb_destroy_result(&res);
    
    duckdb_query(con1, "CREATE VIEW my_orders_view AS SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders')", &res); duckdb_destroy_result(&res);
    
    printf("Starting threads (this will deadlock with the broken code)...\n");
    pthread_t t1, t2;
    pthread_create(&t1, NULL, run_info_schema, NULL);
    pthread_create(&t2, NULL, run_read, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    printf("Success! No deadlock.\n");
    duckdb_disconnect(&con1);
    duckdb_disconnect(&con2);
    duckdb_close(&db);
    return 0;
}
