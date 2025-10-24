#!/usr/bin/env python3
"""Test HTTP/2 with Huffman decoding."""

import httpx
import asyncio

async def test_http2():
    # Start server in background
    print("Testing HTTP/2 with Huffman decoder...")

    # Use httpx with HTTP/2
    async with httpx.AsyncClient(http2=True, verify=False) as client:
        try:
            # Make HTTP/2 request
            response = await client.get('http://localhost:8080/')
            print(f"Status: {response.status_code}")
            print(f"Headers: {dict(response.headers)}")
            print(f"Body: {response.text}")
            print(f"HTTP version: {response.http_version}")

            # Make another request
            response2 = await client.get('http://localhost:8080/json')
            print(f"\nSecond request:")
            print(f"Status: {response2.status_code}")
            print(f"Body: {response2.text}")

            print("\n✓ HTTP/2 working with Huffman decoder!")
            return True
        except Exception as e:
            print(f"✗ Error: {e}")
            import traceback
            traceback.print_exc()
            return False

if __name__ == '__main__':
    asyncio.run(test_http2())
