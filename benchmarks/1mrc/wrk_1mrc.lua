-- 1 Million Request Challenge - wrk benchmark script
--
-- This script generates POST /event requests with randomized JSON payloads.
--
-- Usage:
--   wrk -t8 -c256 -d30s -s benchmarks/1mrc/wrk_1mrc.lua http://localhost:8000
--
-- The script generates:
--   - 75,000 unique users (user_0 to user_74999)
--   - Values from 0.0 to 999.0
--   - Expected sum after N requests: sum = N * 499.5 (average value)
--
-- Note: wrk runs in multiple threads, each with its own counter.
-- The user_id distribution ensures we hit all 75K unique users.

-- Thread-local counter for request generation
local counter = 0
local user_base = 75000  -- 75K unique users

-- Pre-generate request template for efficiency
local req_template = 'POST /event HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s'

-- Called once per thread at thread initialization
function setup(thread)
    -- Initialize counter with random offset to get distribution across users
    counter = math.random(0, user_base - 1)
end

-- Called for each request
function request()
    counter = counter + 1

    -- Generate user_id (cycles through 75K unique users)
    local user_num = counter % user_base
    local user_id = "user_" .. user_num

    -- Generate value (cycles through 0-999, average = 499.5)
    local value = counter % 1000

    -- Build JSON body
    local body = '{"userId":"' .. user_id .. '","value":' .. value .. '}'

    -- Use wrk.format for proper HTTP request building with keep-alive
    return wrk.format("POST", "/event", {
        ["Content-Type"] = "application/json"
    }, body)
end

-- Called after each response (optional - for tracking)
function response(status, headers, body)
    -- Could track errors here if needed
    -- if status ~= 201 then
    --     print("Error: " .. status)
    -- end
end

-- Called at the end of the benchmark (optional)
function done(summary, latency, requests)
    io.write("\n")
    io.write("=== 1MRC Benchmark Complete ===\n")
    io.write(string.format("Total Requests: %d\n", summary.requests))
    io.write(string.format("Duration: %.2fs\n", summary.duration / 1000000))
    io.write(string.format("Requests/sec: %.2f\n", summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Errors: %d (connect: %d, read: %d, write: %d, timeout: %d)\n",
        summary.errors.connect + summary.errors.read + summary.errors.write + summary.errors.timeout,
        summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.timeout))
    io.write("\n")
    io.write("Next: curl http://localhost:8000/stats to verify results\n")
    io.write("\n")
end
