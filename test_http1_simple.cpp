#include <iostream>
#include "src/cpp/http/http1_connection.h"

using namespace fasterapi::http;

int main() {
    std::cout << "Testing Http1Connection directly..." << std::endl;
    
    Http1Connection conn(999);  // Fake FD
    
    conn.set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        std::cout << "[Callback] Creating response..." << std::endl;
        Http1Response response;
        response.status = 200;
        response.body = "Hello World!";
        response.headers["Content-Type"] = "text/plain";
        std::cout << "[Callback] Response created" << std::endl;
        return response;
    });
    
    std::cout << "Simulating HTTP request..." << std::endl;
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto result = conn.process_input(reinterpret_cast<const uint8_t*>(request), strlen(request));
    
    if (result.is_err()) {
        std::cerr << "ERROR: process_input failed!" << std::endl;
        return 1;
    }
    
    std::cout << "Success! Has pending output: " << conn.has_pending_output() << std::endl;
    
    const uint8_t* data;
    size_t len;
    if (conn.get_output(&data, &len)) {
        std::cout << "Response (" << len << " bytes):" << std::endl;
        std::cout.write(reinterpret_cast<const char*>(data), len);
    }
    
    return 0;
}
