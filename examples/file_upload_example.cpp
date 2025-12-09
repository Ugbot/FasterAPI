/**
 * File Upload Example - Multipart file upload handling
 *
 * Demonstrates:
 * - Multipart form-data parsing
 * - File upload handling
 * - File metadata extraction
 * - Multiple file upload
 * - Form field handling
 * - File size limits
 * - File type validation
 *
 * Build:
 *   cmake --build build --target file_upload_example
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/file_upload_example
 *
 * Test:
 *   curl -X POST http://localhost:8080/upload \
 *        -F "file=@/path/to/file.txt" \
 *        -F "description=My file"
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <unordered_set>

using namespace fasterapi;
namespace fs = std::filesystem;

// Upload configuration
const size_t MAX_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
const std::unordered_set<std::string> ALLOWED_TYPES = {
    "text/plain", "text/csv", "application/json",
    "image/jpeg", "image/png", "image/gif",
    "application/pdf", "application/zip"
};

// Generate file size string
std::string format_size(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024 && unit < 3) {
        size /= 1024;
        unit++;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return ss.str();
}

// Simple file extension to MIME type mapping
std::string guess_mime_type(const std::string& filename) {
    auto pos = filename.rfind('.');
    if (pos == std::string::npos) return "application/octet-stream";

    std::string ext = filename.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::unordered_map<std::string, std::string> types = {
        {".txt", "text/plain"},
        {".csv", "text/csv"},
        {".json", "application/json"},
        {".html", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
    };

    auto it = types.find(ext);
    return it != types.end() ? it->second : "application/octet-stream";
}

int main() {
    std::cout << "=== File Upload Example ===" << std::endl;

    // Create uploads directory
    fs::create_directories("./uploads");

    App::Config config;
    config.pure_cpp_mode = true;
    config.max_request_size = MAX_FILE_SIZE + 1024 * 1024;  // Extra for headers
    App app(config);

    // Upload form page
    app.get("/", [](Request& req, Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head>
    <title>File Upload</title>
    <style>
        body { font-family: sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        .form-group { margin: 20px 0; }
        input[type="file"] { display: block; margin: 10px 0; }
        button { padding: 10px 20px; font-size: 16px; cursor: pointer; }
        #result { margin-top: 20px; padding: 10px; border: 1px solid #ccc; display: none; }
        .success { border-color: green; background: #e8f5e9; }
        .error { border-color: red; background: #ffebee; }
    </style>
</head>
<body>
    <h1>File Upload Demo</h1>

    <form id="uploadForm" enctype="multipart/form-data">
        <div class="form-group">
            <label>Select file(s):</label>
            <input type="file" name="files" multiple>
        </div>
        <div class="form-group">
            <label>Description:</label>
            <input type="text" name="description" placeholder="Optional description">
        </div>
        <button type="submit">Upload</button>
    </form>

    <div id="result"></div>

    <h2>Allowed file types:</h2>
    <ul>
        <li>Text: .txt, .csv, .json</li>
        <li>Images: .jpg, .png, .gif</li>
        <li>Documents: .pdf</li>
        <li>Archives: .zip</li>
    </ul>
    <p>Maximum file size: 10 MB</p>

    <script>
        document.getElementById('uploadForm').onsubmit = async (e) => {
            e.preventDefault();
            const formData = new FormData(e.target);

            try {
                const response = await fetch('/upload', {
                    method: 'POST',
                    body: formData
                });
                const result = await response.json();
                const resultDiv = document.getElementById('result');
                resultDiv.style.display = 'block';

                if (response.ok) {
                    resultDiv.className = 'success';
                    resultDiv.innerHTML = '<pre>' + JSON.stringify(result, null, 2) + '</pre>';
                } else {
                    resultDiv.className = 'error';
                    resultDiv.textContent = result.error || 'Upload failed';
                }
            } catch (err) {
                document.getElementById('result').className = 'error';
                document.getElementById('result').textContent = 'Error: ' + err.message;
            }
        };
    </script>
</body>
</html>
)");
    });

    // Single file upload
    app.post("/upload", [](Request& req, Response& res) {
        HttpRequest* raw_req = req.raw();
        if (!raw_req->is_multipart()) {
            res.bad_request().json(R"({"error":"Expected multipart/form-data"})");
            return;
        }

        const auto& files = raw_req->files();

        if (files.empty()) {
            res.bad_request().json(R"({"error":"No files uploaded"})");
            return;
        }

        // Get optional description
        std::string description = raw_req->get_form_field("description");

        std::ostringstream json;
        json << R"({"success":true,"description":")" << description << R"(","files":[)";

        bool first = true;
        for (const auto& file : files) {
            // Validate file size
            if (file.data.size() > MAX_FILE_SIZE) {
                res.status(413).json(R"({"error":"File too large: )" + file.filename + "\"}");
                return;
            }

            // Validate file type
            std::string mime_type = file.content_type.empty() ?
                guess_mime_type(file.filename) : file.content_type;

            if (ALLOWED_TYPES.find(mime_type) == ALLOWED_TYPES.end()) {
                res.bad_request().json(R"({"error":"File type not allowed: )" + mime_type + "\"}");
                return;
            }

            // Save file
            std::string safe_name = std::to_string(std::hash<std::string>{}(file.filename)) +
                                   "_" + file.filename;
            std::string path = "./uploads/" + safe_name;

            std::ofstream out(path, std::ios::binary);
            if (out) {
                out.write(reinterpret_cast<const char*>(file.data.data()), file.data.size());
                out.close();
            }

            if (!first) json << ",";
            json << R"({"filename":")" << file.filename << R"(")"
                 << R"(,"size":")" << format_size(file.data.size()) << R"(")"
                 << R"(,"type":")" << mime_type << R"(")"
                 << R"(,"saved_as":")" << safe_name << R"("})";
            first = false;
        }

        json << "]}";
        res.json(json.str());
    });

    // File info endpoint (without saving)
    app.post("/upload/info", [](Request& req, Response& res) {
        HttpRequest* raw_req = req.raw();
        if (!raw_req->is_multipart()) {
            res.bad_request().json(R"({"error":"Expected multipart/form-data"})");
            return;
        }

        const auto& files = raw_req->files();

        std::ostringstream json;
        json << R"({"files":[)";

        bool first = true;
        for (const auto& file : files) {
            std::string mime_type = file.content_type.empty() ?
                guess_mime_type(file.filename) : file.content_type;

            if (!first) json << ",";
            json << R"({"filename":")" << file.filename << R"(")"
                 << R"(,"field_name":")" << file.name << R"(")"
                 << R"(,"size":)" << file.data.size()
                 << R"(,"size_formatted":")" << format_size(file.data.size()) << R"(")"
                 << R"(,"content_type":")" << mime_type << R"("})";
            first = false;
        }

        json << "]}";
        res.json(json.str());
    });

    // List uploaded files
    app.get("/files", [](Request& req, Response& res) {
        std::ostringstream json;
        json << R"({"files":[)";

        bool first = true;
        for (const auto& entry : fs::directory_iterator("./uploads")) {
            if (entry.is_regular_file()) {
                auto size = entry.file_size();
                auto name = entry.path().filename().string();

                if (!first) json << ",";
                json << R"({"name":")" << name << R"(")"
                     << R"(,"size":")" << format_size(size) << R"("})";
                first = false;
            }
        }

        json << "]}";
        res.json(json.str());
    });

    // Download file
    app.get("/files/{name}", [](Request& req, Response& res) {
        std::string name = req.path_param("name");
        std::string path = "./uploads/" + name;

        if (!fs::exists(path)) {
            res.not_found().json(R"({"error":"File not found"})");
            return;
        }

        res.file(path);
    });

    // Delete file
    app.del("/files/{name}", [](Request& req, Response& res) {
        std::string name = req.path_param("name");
        std::string path = "./uploads/" + name;

        if (fs::remove(path)) {
            res.json(R"({"deleted":")" + name + "\"}");
        } else {
            res.not_found().json(R"({"error":"File not found"})");
        }
    });

    std::cout << "\nStarting on http://localhost:8080" << std::endl;
    std::cout << "\nEndpoints:" << std::endl;
    std::cout << "  GET  /             - Upload form" << std::endl;
    std::cout << "  POST /upload       - Upload file(s)" << std::endl;
    std::cout << "  POST /upload/info  - Get file info without saving" << std::endl;
    std::cout << "  GET  /files        - List uploaded files" << std::endl;
    std::cout << "  GET  /files/{name} - Download file" << std::endl;
    std::cout << "  DELETE /files/{name} - Delete file" << std::endl;
    std::cout << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
