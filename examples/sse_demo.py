"""
Server-Sent Events (SSE) Demo

Demonstrates SSE support in FasterAPI.
Features:
- Real-time event streaming
- Named events
- Event IDs for reconnection
- Keep-alive pings
"""

import time
import json
import random
from datetime import datetime
from fasterapi import App, SSEConnection

app = App(port=8000)


@app.sse("/events")
def event_stream(sse: SSEConnection):
    """
    Simple event stream.
    
    Sends events every second for 30 seconds.
    """
    print("SSE connection opened")
    
    try:
        for i in range(30):
            if not sse.is_open():
                break
            
            # Send event
            sse.send(
                data={"count": i, "timestamp": time.time()},
                event="count",
                id=str(i)
            )
            
            print(f"Sent event {i}")
            time.sleep(1)
            
    except Exception as e:
        print(f"SSE error: {e}")
    
    finally:
        print(f"SSE connection closed (sent {sse.events_sent} events)")


@app.sse("/clock")
def clock_stream(sse: SSEConnection):
    """
    Real-time clock.
    
    Sends current time every second.
    """
    print("Clock SSE opened")
    
    try:
        while sse.is_open():
            now = datetime.now()
            
            sse.send(
                data={
                    "time": now.strftime("%H:%M:%S"),
                    "date": now.strftime("%Y-%m-%d"),
                    "timestamp": now.timestamp()
                },
                event="time",
                id=str(int(now.timestamp()))
            )
            
            time.sleep(1)
    
    except Exception as e:
        print(f"Error: {e}")


@app.sse("/stock-prices")
def stock_prices(sse: SSEConnection):
    """
    Simulated stock prices.
    
    Sends random stock price updates.
    """
    print("Stock prices SSE opened")
    
    stocks = ["AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"]
    prices = {stock: 100.0 for stock in stocks}
    
    try:
        event_id = 0
        
        while sse.is_open():
            # Update random stock
            stock = random.choice(stocks)
            change = random.uniform(-2, 2)
            prices[stock] += change
            
            sse.send(
                data={
                    "symbol": stock,
                    "price": round(prices[stock], 2),
                    "change": round(change, 2)
                },
                event="price",
                id=str(event_id)
            )
            
            event_id += 1
            time.sleep(random.uniform(0.5, 2.0))
    
    except Exception as e:
        print(f"Error: {e}")


@app.sse("/notifications")
def notifications(sse: SSEConnection):
    """
    Notification stream.
    
    Sends periodic notifications with different types.
    """
    print("Notifications SSE opened")
    
    notification_types = ["info", "warning", "error", "success"]
    messages = {
        "info": ["System update available", "New message received", "Reminder set"],
        "warning": ["Low disk space", "High CPU usage", "Network latency detected"],
        "error": ["Failed to save", "Connection lost", "Authentication failed"],
        "success": ["File uploaded", "Settings saved", "Task completed"]
    }
    
    try:
        event_id = 0
        
        while sse.is_open():
            # Random notification
            notif_type = random.choice(notification_types)
            message = random.choice(messages[notif_type])
            
            sse.send(
                data={
                    "type": notif_type,
                    "message": message,
                    "timestamp": datetime.now().isoformat()
                },
                event="notification",
                id=str(event_id)
            )
            
            # Send keep-alive ping occasionally
            if event_id % 10 == 0:
                sse.ping()
            
            event_id += 1
            time.sleep(random.uniform(2, 5))
    
    except Exception as e:
        print(f"Error: {e}")


@app.get("/")
def home(req, res):
    """
    Serve simple HTML client for testing SSE.
    """
    html = """
    <!DOCTYPE html>
    <html>
    <head>
        <title>FasterAPI SSE Demo</title>
        <style>
            body { 
                font-family: Arial, sans-serif; 
                margin: 40px;
                background-color: #f5f5f5;
            }
            .container {
                max-width: 1200px;
                margin: 0 auto;
            }
            h1 { color: #333; }
            .section {
                background: white;
                padding: 20px;
                margin: 20px 0;
                border-radius: 8px;
                box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            }
            button {
                padding: 10px 20px;
                margin: 5px;
                cursor: pointer;
                border: none;
                border-radius: 4px;
                background-color: #007bff;
                color: white;
            }
            button:hover { background-color: #0056b3; }
            button.disconnect { background-color: #dc3545; }
            button.disconnect:hover { background-color: #c82333; }
            #events {
                border: 1px solid #ddd;
                height: 300px;
                overflow-y: auto;
                padding: 10px;
                background: #fafafa;
                font-family: monospace;
                font-size: 12px;
            }
            .event {
                margin: 5px 0;
                padding: 5px;
                border-left: 3px solid #007bff;
                background: white;
            }
            .status { 
                display: inline-block;
                padding: 5px 10px;
                border-radius: 4px;
                font-weight: bold;
            }
            .status.connected { background: #28a745; color: white; }
            .status.disconnected { background: #6c757d; color: white; }
        </style>
    </head>
    <body>
        <div class="container">
            <h1>🚀 FasterAPI SSE Demo</h1>
            
            <div class="section">
                <h2>Counter Stream</h2>
                <button onclick="connectCounter()">Connect</button>
                <button class="disconnect" onclick="disconnectCounter()">Disconnect</button>
                <span id="counterStatus" class="status disconnected">Disconnected</span>
                <div id="counterValue" style="font-size: 24px; margin: 10px 0;">-</div>
            </div>
            
            <div class="section">
                <h2>Clock Stream</h2>
                <button onclick="connectClock()">Connect</button>
                <button class="disconnect" onclick="disconnectClock()">Disconnect</button>
                <span id="clockStatus" class="status disconnected">Disconnected</span>
                <div id="clockValue" style="font-size: 24px; margin: 10px 0;">-</div>
            </div>
            
            <div class="section">
                <h2>Stock Prices</h2>
                <button onclick="connectStocks()">Connect</button>
                <button class="disconnect" onclick="disconnectStocks()">Disconnect</button>
                <span id="stocksStatus" class="status disconnected">Disconnected</span>
                <div id="stocksValue" style="margin: 10px 0;"></div>
            </div>
            
            <div class="section">
                <h2>All Events Log</h2>
                <div id="events"></div>
            </div>
        </div>
        
        <script>
            let counterSource = null;
            let clockSource = null;
            let stocksSource = null;
            
            function connectCounter() {
                if (counterSource) return;
                
                counterSource = new EventSource('/events');
                
                counterSource.addEventListener('count', (e) => {
                    const data = JSON.parse(e.data);
                    document.getElementById('counterValue').textContent = `Count: ${data.count}`;
                    logEvent('Counter', data);
                });
                
                counterSource.onopen = () => {
                    document.getElementById('counterStatus').textContent = 'Connected';
                    document.getElementById('counterStatus').className = 'status connected';
                };
                
                counterSource.onerror = () => {
                    document.getElementById('counterStatus').textContent = 'Error';
                    document.getElementById('counterStatus').className = 'status disconnected';
                };
            }
            
            function disconnectCounter() {
                if (counterSource) {
                    counterSource.close();
                    counterSource = null;
                    document.getElementById('counterStatus').textContent = 'Disconnected';
                    document.getElementById('counterStatus').className = 'status disconnected';
                }
            }
            
            function connectClock() {
                if (clockSource) return;
                
                clockSource = new EventSource('/clock');
                
                clockSource.addEventListener('time', (e) => {
                    const data = JSON.parse(e.data);
                    document.getElementById('clockValue').textContent = `${data.time} ${data.date}`;
                    logEvent('Clock', data);
                });
                
                clockSource.onopen = () => {
                    document.getElementById('clockStatus').textContent = 'Connected';
                    document.getElementById('clockStatus').className = 'status connected';
                };
                
                clockSource.onerror = () => {
                    document.getElementById('clockStatus').textContent = 'Error';
                    document.getElementById('clockStatus').className = 'status disconnected';
                };
            }
            
            function disconnectClock() {
                if (clockSource) {
                    clockSource.close();
                    clockSource = null;
                    document.getElementById('clockStatus').textContent = 'Disconnected';
                    document.getElementById('clockStatus').className = 'status disconnected';
                }
            }
            
            function connectStocks() {
                if (stocksSource) return;
                
                const prices = {};
                
                stocksSource = new EventSource('/stock-prices');
                
                stocksSource.addEventListener('price', (e) => {
                    const data = JSON.parse(e.data);
                    prices[data.symbol] = data.price;
                    
                    let html = '<table style="width:100%"><tr><th>Symbol</th><th>Price</th></tr>';
                    for (const [symbol, price] of Object.entries(prices)) {
                        html += `<tr><td>${symbol}</td><td>$${price.toFixed(2)}</td></tr>`;
                    }
                    html += '</table>';
                    
                    document.getElementById('stocksValue').innerHTML = html;
                    logEvent('Stock', data);
                });
                
                stocksSource.onopen = () => {
                    document.getElementById('stocksStatus').textContent = 'Connected';
                    document.getElementById('stocksStatus').className = 'status connected';
                };
                
                stocksSource.onerror = () => {
                    document.getElementById('stocksStatus').textContent = 'Error';
                    document.getElementById('stocksStatus').className = 'status disconnected';
                };
            }
            
            function disconnectStocks() {
                if (stocksSource) {
                    stocksSource.close();
                    stocksSource = null;
                    document.getElementById('stocksStatus').textContent = 'Disconnected';
                    document.getElementById('stocksStatus').className = 'status disconnected';
                }
            }
            
            function logEvent(source, data) {
                const events = document.getElementById('events');
                const div = document.createElement('div');
                div.className = 'event';
                div.textContent = `[${new Date().toLocaleTimeString()}] ${source}: ${JSON.stringify(data)}`;
                events.appendChild(div);
                events.scrollTop = events.scrollHeight;
                
                // Keep only last 50 events
                while (events.children.length > 50) {
                    events.removeChild(events.firstChild);
                }
            }
        </script>
    </body>
    </html>
    """
    
    res.set_header("Content-Type", "text/html")
    return html


def main():
    print("╔══════════════════════════════════════════╗")
    print("║     FasterAPI SSE Demo                   ║")
    print("╚══════════════════════════════════════════╝")
    print()
    print("Server starting on http://localhost:8000")
    print()
    print("SSE endpoints:")
    print("  /events         - Counter (30 events)")
    print("  /clock          - Real-time clock")
    print("  /stock-prices   - Simulated stock prices")
    print("  /notifications  - Random notifications")
    print()
    print("Open http://localhost:8000 in your browser to test")
    print()
    
    app.run()


if __name__ == "__main__":
    main()



