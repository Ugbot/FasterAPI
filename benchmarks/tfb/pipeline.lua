-- TechEmpower Framework Benchmarks pipeline.lua
-- Creates pipelined HTTP requests for maximum throughput testing
--
-- Usage: wrk -s pipeline.lua -- <depth>
-- Example: wrk -t8 -c256 -d15s -s pipeline.lua http://localhost:8080/plaintext -- 16

init = function(args)
    local r = {}
    local depth = tonumber(args[1]) or 1
    for i = 1, depth do
        r[i] = wrk.format()
    end
    req = table.concat(r)
end

request = function()
    return req
end
