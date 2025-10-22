#!/usr/bin/env python3.13
"""Simple connection test for the HTTP server"""
import socket
import sys

def test_connection():
    """Test that the server accepts TCP connections"""
    print("=" * 70)
    print("HTTP Server Connection Test")
    print("=" * 70)
    print()

    try:
        # Create socket and connect
        print("Creating TCP socket...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(2.0)

        print("Connecting to localhost:8000...")
        sock.connect(('127.0.0.1', 8000))
        print("✓ Connected successfully!")
        print()

        # Send HTTP request
        request = b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n'
        print(f"Sending HTTP request ({len(request)} bytes)...")
        sock.send(request)
        print("✓ Request sent!")
        print()

        # Try to receive response (will timeout since handlers not implemented)
        print("Waiting for response (2 second timeout)...")
        try:
            response = sock.recv(4096)
            if response:
                print(f"✓ Received {len(response)} bytes:")
                print(response.decode('utf-8', errors='ignore'))
            else:
                print("⚠ Connection closed without response")
        except socket.timeout:
            print("⚠ No response (timeout) - Python handlers not yet implemented")
            print("  This is EXPECTED - the server accepts connections but can't respond yet")

        sock.close()
        print()
        print("=" * 70)
        print("SUCCESS: HTTP server is listening and accepting connections!")
        print("=" * 70)
        print()
        print("Next step: Implement Python callback mechanism for route handlers")
        return True

    except ConnectionRefusedError:
        print("✗ Connection refused - server not running on port 8000")
        return False
    except Exception as e:
        print(f"✗ Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    success = test_connection()
    sys.exit(0 if success else 1)
