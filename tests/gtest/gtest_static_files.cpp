/**
 * Static File Serving Unit Tests
 *
 * Tests the C++ static file serving implementation:
 * - MIME type detection for various file extensions
 * - File reading and response body
 * - Caching headers (ETag, Last-Modified, Cache-Control)
 * - 404 handling for non-existent files
 * - Directory traversal prevention (security)
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/response.h"
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <random>
#include <sstream>

namespace fasterapi {
namespace http {
namespace test {

class StaticFilesTest : public ::testing::Test {
protected:
    std::string test_dir_;
    std::vector<std::string> created_files_;

    void SetUp() override {
        // Create a temporary test directory
        test_dir_ = "/tmp/fasterapi_static_test_" + std::to_string(getpid());
        mkdir(test_dir_.c_str(), 0755);

        // Create subdirectory
        std::string subdir = test_dir_ + "/subdir";
        mkdir(subdir.c_str(), 0755);
    }

    void TearDown() override {
        // Clean up created files
        for (const auto& file : created_files_) {
            unlink(file.c_str());
        }
        // Remove subdirectory and test directory
        std::string subdir = test_dir_ + "/subdir";
        rmdir(subdir.c_str());
        rmdir(test_dir_.c_str());
    }

    void create_test_file(const std::string& filename, const std::string& content) {
        std::string path = test_dir_ + "/" + filename;
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(content.data(), content.size());
        ofs.close();
        created_files_.push_back(path);
    }

    void create_binary_file(const std::string& filename, const std::vector<uint8_t>& content) {
        std::string path = test_dir_ + "/" + filename;
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(content.data()), content.size());
        ofs.close();
        created_files_.push_back(path);
    }

    std::string random_string(size_t length) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return result;
    }
};

// ============================================================================
// MIME Type Detection Tests
// ============================================================================

TEST_F(StaticFilesTest, MimeTypeHtml) {
    create_test_file("index.html", "<!DOCTYPE html><html><body>Hello</body></html>");

    HttpResponse res;
    res.file(test_dir_ + "/index.html");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("text/html"), std::string::npos) << "HTML file should have text/html content type";
}

TEST_F(StaticFilesTest, MimeTypeCss) {
    create_test_file("style.css", "body { background: blue; }");

    HttpResponse res;
    res.file(test_dir_ + "/style.css");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("text/css"), std::string::npos) << "CSS file should have text/css content type";
}

TEST_F(StaticFilesTest, MimeTypeJavaScript) {
    create_test_file("script.js", "console.log('hello');");

    HttpResponse res;
    res.file(test_dir_ + "/script.js");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("javascript"), std::string::npos) << "JS file should have javascript content type";
}

TEST_F(StaticFilesTest, MimeTypeJson) {
    create_test_file("data.json", "{\"key\": \"value\"}");

    HttpResponse res;
    res.file(test_dir_ + "/data.json");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("application/json"), std::string::npos) << "JSON file should have application/json content type";
}

TEST_F(StaticFilesTest, MimeTypePng) {
    // PNG magic bytes
    std::vector<uint8_t> png_data = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    create_binary_file("image.png", png_data);

    HttpResponse res;
    res.file(test_dir_ + "/image.png");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("image/png"), std::string::npos) << "PNG file should have image/png content type";
}

TEST_F(StaticFilesTest, MimeTypeJpeg) {
    // JPEG magic bytes (SOI marker)
    std::vector<uint8_t> jpg_data = {0xFF, 0xD8, 0xFF, 0xE0};
    create_binary_file("photo.jpg", jpg_data);

    HttpResponse res;
    res.file(test_dir_ + "/photo.jpg");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("image/jpeg"), std::string::npos) << "JPEG file should have image/jpeg content type";
}

TEST_F(StaticFilesTest, MimeTypeSvg) {
    create_test_file("icon.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>");

    HttpResponse res;
    res.file(test_dir_ + "/icon.svg");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("image/svg+xml"), std::string::npos) << "SVG file should have image/svg+xml content type";
}

TEST_F(StaticFilesTest, MimeTypeWoff2) {
    std::vector<uint8_t> woff2_data = {0x77, 0x4F, 0x46, 0x32};  // wOF2
    create_binary_file("font.woff2", woff2_data);

    HttpResponse res;
    res.file(test_dir_ + "/font.woff2");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("font/woff2"), std::string::npos) << "WOFF2 file should have font/woff2 content type";
}

TEST_F(StaticFilesTest, MimeTypeWasm) {
    // WebAssembly magic bytes
    std::vector<uint8_t> wasm_data = {0x00, 0x61, 0x73, 0x6D};  // \0asm
    create_binary_file("module.wasm", wasm_data);

    HttpResponse res;
    res.file(test_dir_ + "/module.wasm");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("application/wasm"), std::string::npos) << "WASM file should have application/wasm content type";
}

TEST_F(StaticFilesTest, MimeTypeUnknownExtension) {
    create_test_file("data.xyz", "some random data");

    HttpResponse res;
    res.file(test_dir_ + "/data.xyz");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("application/octet-stream"), std::string::npos)
        << "Unknown extension should default to application/octet-stream";
}

// ============================================================================
// File Content Tests
// ============================================================================

TEST_F(StaticFilesTest, FileContentCorrect) {
    std::string content = "Hello, World! This is test content.";
    create_test_file("test.txt", content);

    HttpResponse res;
    res.file(test_dir_ + "/test.txt");

    std::string wire = res.to_http_wire_format(true);

    // The content should appear after the headers (after \r\n\r\n)
    size_t body_start = wire.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string body = wire.substr(body_start + 4);

    EXPECT_EQ(body, content) << "File content should match exactly";
}

TEST_F(StaticFilesTest, BinaryFileContentCorrect) {
    std::vector<uint8_t> binary_content;
    for (int i = 0; i < 256; ++i) {
        binary_content.push_back(static_cast<uint8_t>(i));
    }
    create_binary_file("binary.bin", binary_content);

    HttpResponse res;
    res.file(test_dir_ + "/binary.bin");

    std::string wire = res.to_http_wire_format(true);

    size_t body_start = wire.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string body = wire.substr(body_start + 4);

    EXPECT_EQ(body.size(), 256) << "Binary file size should match";
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(static_cast<uint8_t>(body[i]), static_cast<uint8_t>(i))
            << "Binary content mismatch at byte " << i;
    }
}

TEST_F(StaticFilesTest, LargeFileContent) {
    // Create a 1MB file with random content
    std::string content = random_string(1024 * 1024);
    create_test_file("large.txt", content);

    HttpResponse res;
    res.file(test_dir_ + "/large.txt");

    std::string wire = res.to_http_wire_format(true);

    size_t body_start = wire.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string body = wire.substr(body_start + 4);

    EXPECT_EQ(body.size(), content.size()) << "Large file size should match";
    EXPECT_EQ(body, content) << "Large file content should match";
}

TEST_F(StaticFilesTest, SubdirectoryFile) {
    create_test_file("subdir/nested.txt", "nested content");

    HttpResponse res;
    res.file(test_dir_ + "/subdir/nested.txt");

    std::string wire = res.to_http_wire_format(true);

    size_t body_start = wire.find("\r\n\r\n");
    ASSERT_NE(body_start, std::string::npos);
    std::string body = wire.substr(body_start + 4);

    EXPECT_EQ(body, "nested content");
}

// ============================================================================
// 404 Not Found Tests
// ============================================================================

TEST_F(StaticFilesTest, FileNotFound) {
    HttpResponse res;
    res.file(test_dir_ + "/nonexistent.txt");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("404"), std::string::npos) << "Non-existent file should return 404";
}

TEST_F(StaticFilesTest, DirectoryAsFile) {
    // Trying to serve a directory as a file should fail
    HttpResponse res;
    res.file(test_dir_ + "/subdir");

    std::string wire = res.to_http_wire_format(true);
    // Should be 404 since it's not a regular file
    EXPECT_NE(wire.find("404"), std::string::npos) << "Directory should return 404";
}

// ============================================================================
// Security Tests - Directory Traversal
// ============================================================================

TEST_F(StaticFilesTest, DirectoryTraversalBlocked) {
    HttpResponse res;
    res.file(test_dir_ + "/../../../etc/passwd");

    std::string wire = res.to_http_wire_format(true);

    // Should be 403 Forbidden or similar error, not successful
    bool is_blocked = wire.find("403") != std::string::npos ||
                      wire.find("400") != std::string::npos ||
                      wire.find("404") != std::string::npos;
    EXPECT_TRUE(is_blocked) << "Directory traversal should be blocked";

    // Make sure /etc/passwd content is NOT in response
    EXPECT_EQ(wire.find("root:"), std::string::npos) << "Should not leak /etc/passwd content";
}

TEST_F(StaticFilesTest, EncodedTraversalBlocked) {
    // Try URL-encoded traversal (though this would normally be decoded by the router)
    HttpResponse res;
    res.file(test_dir_ + "/..%2F..%2Fetc/passwd");

    std::string wire = res.to_http_wire_format(true);

    // Should fail safely
    EXPECT_EQ(wire.find("root:"), std::string::npos) << "Encoded traversal should be blocked";
}

// ============================================================================
// Caching Header Tests
// ============================================================================

TEST_F(StaticFilesTest, CacheControlHeader) {
    create_test_file("cached.txt", "cacheable content");

    HttpResponse res;
    res.file(test_dir_ + "/cached.txt");

    std::string wire = res.to_http_wire_format(true);

    // Should have Cache-Control header
    EXPECT_NE(wire.find("cache-control:"), std::string::npos)
        << "Should have Cache-Control header";
}

TEST_F(StaticFilesTest, ETagHeader) {
    create_test_file("etag.txt", "content for etag test");

    HttpResponse res;
    res.file(test_dir_ + "/etag.txt");

    std::string wire = res.to_http_wire_format(true);

    // Should have ETag header
    EXPECT_NE(wire.find("etag:"), std::string::npos) << "Should have ETag header";
}

TEST_F(StaticFilesTest, LastModifiedHeader) {
    create_test_file("modified.txt", "content for last-modified test");

    HttpResponse res;
    res.file(test_dir_ + "/modified.txt");

    std::string wire = res.to_http_wire_format(true);

    // Should have Last-Modified header
    EXPECT_NE(wire.find("last-modified:"), std::string::npos)
        << "Should have Last-Modified header";
}

TEST_F(StaticFilesTest, ETagChangesWithContent) {
    create_test_file("changing.txt", "original content");

    HttpResponse res1;
    res1.file(test_dir_ + "/changing.txt");
    std::string wire1 = res1.to_http_wire_format(true);

    // Extract ETag from first response
    size_t etag_pos = wire1.find("etag:");
    ASSERT_NE(etag_pos, std::string::npos);
    size_t etag_end = wire1.find("\r\n", etag_pos);
    std::string etag1 = wire1.substr(etag_pos, etag_end - etag_pos);

    // Modify the file
    std::string path = test_dir_ + "/changing.txt";
    std::ofstream ofs(path, std::ios::binary);
    ofs << "modified content that is different";
    ofs.close();

    // Sleep briefly to ensure mtime changes
    usleep(100000);  // 100ms

    HttpResponse res2;
    res2.file(test_dir_ + "/changing.txt");
    std::string wire2 = res2.to_http_wire_format(true);

    // Extract ETag from second response
    etag_pos = wire2.find("etag:");
    ASSERT_NE(etag_pos, std::string::npos);
    etag_end = wire2.find("\r\n", etag_pos);
    std::string etag2 = wire2.substr(etag_pos, etag_end - etag_pos);

    // ETags should be different (based on size + mtime)
    EXPECT_NE(etag1, etag2) << "ETag should change when file content changes";
}

// ============================================================================
// Content-Length Tests
// ============================================================================

TEST_F(StaticFilesTest, ContentLengthCorrect) {
    std::string content = "Exactly 25 bytes content!";
    ASSERT_EQ(content.size(), 25);
    create_test_file("sized.txt", content);

    HttpResponse res;
    res.file(test_dir_ + "/sized.txt");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("Content-Length: 25"), std::string::npos)
        << "Content-Length should be exact";
}

TEST_F(StaticFilesTest, EmptyFile) {
    create_test_file("empty.txt", "");

    HttpResponse res;
    res.file(test_dir_ + "/empty.txt");

    std::string wire = res.to_http_wire_format(true);
    EXPECT_NE(wire.find("Content-Length: 0"), std::string::npos)
        << "Empty file should have Content-Length: 0";
    EXPECT_NE(wire.find("200"), std::string::npos) << "Empty file should return 200 OK";
}

// ============================================================================
// Multiple File Types Randomized Test
// ============================================================================

TEST_F(StaticFilesTest, RandomizedMultipleFiles) {
    // Create multiple files with random content
    std::vector<std::pair<std::string, std::string>> files = {
        {"random1.html", random_string(1000)},
        {"random2.css", random_string(500)},
        {"random3.js", random_string(2000)},
        {"random4.json", "{\"data\": \"" + random_string(100) + "\"}"},
        {"random5.txt", random_string(750)}
    };

    for (const auto& [filename, content] : files) {
        create_test_file(filename, content);

        HttpResponse res;
        res.file(test_dir_ + "/" + filename);

        std::string wire = res.to_http_wire_format(true);

        // Verify 200 OK
        EXPECT_NE(wire.find("200"), std::string::npos)
            << "File " << filename << " should return 200 OK";

        // Verify content
        size_t body_start = wire.find("\r\n\r\n");
        ASSERT_NE(body_start, std::string::npos);
        std::string body = wire.substr(body_start + 4);

        EXPECT_EQ(body, content) << "Content mismatch for " << filename;
    }
}

}  // namespace test
}  // namespace http
}  // namespace fasterapi

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
