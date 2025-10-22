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
		return
	}
	defer conn.Close()

	message := []byte("BENCH\n")
	buffer := make([]byte, 1024)

	start := time.Now()
	for time.Since(start) < duration {
		_, err := conn.Write(message)
		if err != nil {
			return
		}

		n, err := conn.Read(buffer)
		if err != nil {
			return
		}

		if n > 0 {
			counter.Add(1)
		}
	}
}

func runBench(concurrency int) {
	addr := "localhost:8070"
	duration := 10 * time.Second

	var counter atomic.Int64
	var wg sync.WaitGroup

	start := time.Now()

	for i := 0; i < concurrency; i++ {
		wg.Add(1)
		go worker(addr, duration, &wg, &counter)
	}

	wg.Wait()
	elapsed := time.Since(start)

	totalRequests := counter.Load()
	rps := float64(totalRequests) / elapsed.Seconds()

	fmt.Printf("Concurrency %4d: %10d requests in %v = %10.2f req/s\n",
		concurrency, totalRequests, elapsed.Round(time.Millisecond), rps)
}

func main() {
	fmt.Println("Echo Server Performance Benchmark")
	fmt.Println("Testing different concurrency levels...")
	fmt.Println()

	concurrencyLevels := []int{50, 100, 200, 500, 1000}

	for _, c := range concurrencyLevels {
		runBench(c)
		time.Sleep(1 * time.Second)
	}
}
