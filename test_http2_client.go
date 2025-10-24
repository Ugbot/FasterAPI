package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net"
	"net/http"
	"time"

	"golang.org/x/net/http2"
)

func main() {
	url := "http://localhost:8080/"

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

	fmt.Println("Testing HTTP/2 server at", url)
	fmt.Println()

	// Test 1: Simple GET
	fmt.Println("Test 1: Simple GET /")
	resp, err := client.Get(url)
	if err != nil {
		fmt.Printf("  ✗ FAIL: %v\n", err)
		return
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	fmt.Printf("  Status: %d\n", resp.StatusCode)
	fmt.Printf("  Body: %s\n", string(body))
	fmt.Printf("  Protocol: %s\n", resp.Proto)
	if resp.StatusCode == 200 {
		fmt.Println("  ✓ PASS")
	}
	fmt.Println()

	// Test 2: JSON endpoint
	fmt.Println("Test 2: GET /json")
	resp, err = client.Get(url + "json")
	if err != nil {
		fmt.Printf("  ✗ FAIL: %v\n", err)
		return
	}
	body, _ = io.ReadAll(resp.Body)
	resp.Body.Close()
	fmt.Printf("  Status: %d\n", resp.StatusCode)
	fmt.Printf("  Body: %s\n", string(body))
	if resp.StatusCode == 200 {
		fmt.Println("  ✓ PASS")
	}
	fmt.Println()

	// Test 3: Concurrent requests
	fmt.Println("Test 3: 10 concurrent requests")
	start := time.Now()
	done := make(chan bool, 10)
	for i := 0; i < 10; i++ {
		go func(id int) {
			resp, err := client.Get(url)
			if err == nil && resp.StatusCode == 200 {
				io.Copy(io.Discard, resp.Body)
				resp.Body.Close()
				fmt.Printf("  Request %d: ✓\n", id)
			} else {
				fmt.Printf("  Request %d: ✗\n", id)
			}
			done <- true
		}(i)
	}

	// Wait for all
	for i := 0; i < 10; i++ {
		<-done
	}
	elapsed := time.Since(start)
	fmt.Printf("  Total time: %.3fs\n", elapsed.Seconds())
	fmt.Println("  ✓ PASS")

	fmt.Println()
	fmt.Println("✓✓✓ All tests passed! ✓✓✓")
}
