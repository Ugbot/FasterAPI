/**
 * Streaming & SSE Example - Server-Sent Events and response streaming
 *
 * Demonstrates:
 * - Server-Sent Events (SSE) for real-time updates
 * - Chunked response streaming
 * - Long-polling alternative
 * - Event broadcast patterns
 * - Progress streaming
 *
 * Build:
 *   cmake --build build --target streaming_example
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/streaming_example
 *
 * Test:
 *   curl -N http://localhost:8080/events      # SSE stream
 *   curl http://localhost:8080/stream/data    # Chunked response
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

using namespace fasterapi;

// Simulated sensor data generator
class SensorSimulator {
public:
    struct Reading {
        double temperature;
        double humidity;
        double pressure;
        uint64_t timestamp;

        std::string to_json() const {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2);
            ss << R"({"temperature":)" << temperature
               << R"(,"humidity":)" << humidity
               << R"(,"pressure":)" << pressure
               << R"(,"timestamp":)" << timestamp << "}";
            return ss.str();
        }
    };

    Reading read() {
        // Simulate sensor readings with some random variation
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::normal_distribution<> temp_dist(22.0, 2.0);
        static std::normal_distribution<> humidity_dist(50.0, 10.0);
        static std::normal_distribution<> pressure_dist(1013.25, 5.0);

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        return Reading{
            temp_dist(gen),
            humidity_dist(gen),
            pressure_dist(gen),
            static_cast<uint64_t>(ms)
        };
    }
};

int main() {
    std::cout << "=== Streaming & SSE Example ===" << std::endl;

    SensorSimulator sensor;
    std::atomic<int> event_id{0};

    App::Config config;
    config.pure_cpp_mode = true;
    App app(config);

    // Home page with EventSource JavaScript
    app.get("/", [](Request& req, Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head>
    <title>Streaming Example</title>
    <style>
        body { font-family: sans-serif; padding: 20px; }
        .event { background: #f0f0f0; padding: 10px; margin: 5px 0; border-radius: 4px; }
        #events { max-height: 400px; overflow-y: auto; }
        .temp { color: #d35400; }
        .humidity { color: #2980b9; }
        .pressure { color: #27ae60; }
    </style>
</head>
<body>
    <h1>Real-Time Sensor Data</h1>
    <p>Connection status: <span id="status">Connecting...</span></p>

    <h2>Latest Readings</h2>
    <div id="current">
        <p class="temp">Temperature: --</p>
        <p class="humidity">Humidity: --</p>
        <p class="pressure">Pressure: --</p>
    </div>

    <h2>Event Log</h2>
    <div id="events"></div>

    <script>
        const evtSource = new EventSource('/events');

        evtSource.onopen = () => {
            document.getElementById('status').textContent = 'Connected';
            document.getElementById('status').style.color = 'green';
        };

        evtSource.onmessage = (e) => {
            const data = JSON.parse(e.data);

            // Update current readings
            document.querySelector('.temp').textContent =
                'Temperature: ' + data.temperature.toFixed(2) + ' °C';
            document.querySelector('.humidity').textContent =
                'Humidity: ' + data.humidity.toFixed(2) + ' %';
            document.querySelector('.pressure').textContent =
                'Pressure: ' + data.pressure.toFixed(2) + ' hPa';

            // Add to event log
            const div = document.createElement('div');
            div.className = 'event';
            div.textContent = JSON.stringify(data);
            const events = document.getElementById('events');
            events.insertBefore(div, events.firstChild);

            // Limit log size
            while (events.children.length > 20) {
                events.removeChild(events.lastChild);
            }
        };

        evtSource.onerror = () => {
            document.getElementById('status').textContent = 'Disconnected - Retrying...';
            document.getElementById('status').style.color = 'red';
        };
    </script>
</body>
</html>
)");
    });

    // SSE endpoint - sends sensor data every second
    app.sse("/events", [&sensor, &event_id](http::SSEConnection& sse) {
        std::cout << "[SSE] Client connected" << std::endl;

        // Send events for 60 seconds
        for (int i = 0; i < 60; i++) {
            auto reading = sensor.read();
            int id = event_id++;

            // Send SSE event
            sse.send(reading.to_json(), "sensor", std::to_string(id).c_str());

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "[SSE] Stream complete" << std::endl;
    });

    // Chunked streaming - simulates large data download
    app.get("/stream/data", [](Request& req, Response& res) {
        res.stream_start();
        res.header("Content-Type", "application/octet-stream");
        res.header("X-Content-Info", "Simulated large file download");

        // Simulate streaming large chunks of data
        for (int i = 0; i < 10; i++) {
            std::string chunk = "Chunk " + std::to_string(i + 1) +
                               ": " + std::string(100, 'X') + "\n";
            res.stream_chunk(chunk);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        res.stream_end();
    });

    // Progress streaming - simulates task progress
    app.get("/stream/progress", [](Request& req, Response& res) {
        res.content_type("text/event-stream");
        res.header("Cache-Control", "no-cache");
        res.header("Connection", "keep-alive");

        res.stream_start();

        for (int progress = 0; progress <= 100; progress += 10) {
            std::ostringstream ss;
            ss << "data: {\"progress\":" << progress
               << ",\"status\":\"" << (progress < 100 ? "processing" : "complete")
               << "\"}\n\n";
            res.stream_chunk(ss.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        res.stream_end();
    });

    // JSON Lines streaming (NDJSON)
    app.get("/stream/json-lines", [&sensor](Request& req, Response& res) {
        res.content_type("application/x-ndjson");
        res.stream_start();

        // Stream 20 readings as JSON Lines
        for (int i = 0; i < 20; i++) {
            auto reading = sensor.read();
            res.stream_chunk(reading.to_json() + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        res.stream_end();
    });

    // Long-polling endpoint
    app.get("/poll", [&sensor](Request& req, Response& res) {
        // Simulate waiting for new data (up to 30 seconds)
        auto timeout_str = req.query_param_optional("timeout").value_or("30");
        int timeout = std::min(std::stoi(timeout_str), 30);

        // For demo, just wait 1 second and return data
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto reading = sensor.read();
        res.json(reading.to_json());
    });

    std::cout << "\nStarting on http://localhost:8080" << std::endl;
    std::cout << "\nEndpoints:" << std::endl;
    std::cout << "  GET /             - Interactive SSE demo page" << std::endl;
    std::cout << "  GET /events       - SSE sensor data stream" << std::endl;
    std::cout << "  GET /stream/data  - Chunked data stream" << std::endl;
    std::cout << "  GET /stream/progress - Progress events" << std::endl;
    std::cout << "  GET /stream/json-lines - NDJSON stream" << std::endl;
    std::cout << "  GET /poll         - Long-polling endpoint" << std::endl;
    std::cout << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
