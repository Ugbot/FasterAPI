package main

import (
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

func worker(addr string, duration time.Duration, wg *sync.WaitGroup, counter *atomic.Int64) {
	defer wg.Done()

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		fmt.Printf("Connection error: %v\n", err)
		return
	}
	defer conn.Close()

	message := []byte("BENCH\n")
	buffer := make([]byte, 1024)

	start := time.Now()
	for time.Since(start) < duration {
		// Send message
		_, err := conn.Write(message)
		if err != nil {
			return
		}

		// Read echo response
		n, err := conn.Read(buffer)
		if err != nil {
			return
		}

		if n > 0 {
			counter.Add(1)
		}
	}
}

func main() {
	addr := "localhost:8070"
	concurrency := 100
	duration := 10 * time.Second

	fmt.Printf("Benchmarking echo server at %s\n", addr)
	fmt.Printf("Concurrency: %d connections\n", concurrency)
	fmt.Printf("Duration: %v\n", duration)
	fmt.Println("Starting benchmark...")

	var counter atomic.Int64
	var wg sync.WaitGroup

	start := time.Now()

	// Launch concurrent workers
	for i := 0; i < concurrency; i++ {
		wg.Add(1)
		go worker(addr, duration, &wg, &counter)
	}

	// Wait for all workers to finish
	wg.Wait()
	elapsed := time.Since(start)

	totalRequests := counter.Load()
	rps := float64(totalRequests) / elapsed.Seconds()

	fmt.Println("\nResults:")
	fmt.Printf("Total requests: %d\n", totalRequests)
	fmt.Printf("Time elapsed: %v\n", elapsed)
	fmt.Printf("Requests/sec: %.2f\n", rps)
}
