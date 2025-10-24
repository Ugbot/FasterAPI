package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/net/http2"
)

func worker(client *http.Client, url string, duration time.Duration, wg *sync.WaitGroup, counter *atomic.Int64) {
	defer wg.Done()

	start := time.Now()
	for time.Since(start) < duration {
		resp, err := client.Get(url)
		if err != nil {
			continue
		}

		// Read and discard body
		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()

		if resp.StatusCode == 200 {
			counter.Add(1)
		}
	}
}

func main() {
	url := "http://localhost:8080/"
	concurrency := 100
	duration := 10 * time.Second

	fmt.Printf("Benchmarking HTTP/2 server at %s\n", url)
	fmt.Printf("Concurrency: %d connections\n", concurrency)
	fmt.Printf("Duration: %v\n", duration)
	fmt.Println("Starting benchmark...")

	// Create HTTP/2 transport with h2c (HTTP/2 cleartext)
	transport := &http2.Transport{
		AllowHTTP: true,
		DialTLS: func(network, addr string, cfg *tls.Config) (net.Conn, error) {
			// Use regular TCP connection for h2c
			return net.Dial(network, addr)
		},
	}

	client := &http.Client{
		Transport: transport,
		Timeout:   5 * time.Second,
	}

	var counter atomic.Int64
	var wg sync.WaitGroup

	start := time.Now()

	// Launch concurrent workers
	for i := 0; i < concurrency; i++ {
		wg.Add(1)
		go worker(client, url, duration, &wg, &counter)
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
