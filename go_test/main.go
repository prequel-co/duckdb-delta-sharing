package main

import (
	"database/sql"
	"fmt"
	"log"
	"os"
	"sync"
	"time"

	_ "github.com/marcboeker/go-duckdb"
)

func main() {
	endpoint := os.Getenv("DELTA_SHARING_ENDPOINT")
	token := os.Getenv("DELTA_SHARING_BEARER_TOKEN")

	db, err := sql.Open("duckdb", "?access_mode=read_write&allow_unsigned_extensions=true")
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()
	db.SetMaxOpenConns(10)

	_, err = db.Exec("LOAD '../build/release/extension/duckdb_delta_sharing/duckdb_delta_sharing.duckdb_extension'")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("LOAD httpfs")
	if err != nil {
		log.Fatal(err)
	}
	
	secretQuery := fmt.Sprintf("CREATE SECRET (TYPE delta_sharing, PROVIDER config, ENDPOINT '%s', BEARER_TOKEN '%s')", endpoint, token)
	_, err = db.Exec(secretQuery)
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println("Starting highly concurrent deadlock test in Go...")
	var wg sync.WaitGroup

	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for j := 0; j < 50; j++ {
				viewName := fmt.Sprintf("my_view_%d_%d", id, j)
				db.Exec(fmt.Sprintf("CREATE OR REPLACE VIEW %s AS SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders')", viewName))
			}
		}(i)
	}

	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 50; j++ {
				rows, _ := db.Query("SELECT * FROM information_schema.columns")
				if rows != nil {
					rows.Close()
				}
			}
		}()
	}

	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 50; j++ {
				rows, _ := db.Query("SELECT * FROM delta_share_read('prequel_dev_share', 'prequel_dev', 'orders') LIMIT 1")
				if rows != nil {
					rows.Close()
				}
			}
		}()
	}

	c := make(chan struct{})
	go func() {
		wg.Wait()
		close(c)
	}()

	select {
	case <-c:
		fmt.Println("Success: No deadlock detected.")
	case <-time.After(10 * time.Second):
		fmt.Println("TIMEOUT: Deadlock detected!")
		os.Exit(1)
	}
}
