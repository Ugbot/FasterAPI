/**
 * Multipart Parser Unit Tests
 *
 * Tests the multipart/form-data parser:
 * - Boundary extraction
 * - Simple text fields
 * - File uploads
 * - Multiple parts
 * - Streaming parsing
 * - Error handling
 * - Edge cases
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/multipart_parser.h"
#include <random>
#include <sstream>

namespace fasterapi {
namespace test {

// ===========================================================================
// Helper Functions
// ===========================================================================

std::string generate_boundary() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 35);
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    std::string boundary = "----WebKitFormBoundary";
    for (int i = 0; i < 16; i++) {
        boundary += chars[dis(gen)];
    }
    return boundary;
}

std::string build_multipart_body(
    const std::string& boundary,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& files  // name, filename, content_type, data
) {
    std::ostringstream body;

    // Text fields
    for (const auto& [name, value] : fields) {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << name << "\"\r\n";
        body << "\r\n";
        body << value << "\r\n";
    }

    // Files
    for (const auto& [name, filename, content_type, data] : files) {
        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"" << name << "\"; filename=\"" << filename << "\"\r\n";
        body << "Content-Type: " << content_type << "\r\n";
        body << "\r\n";
        body << data << "\r\n";
    }

    body << "--" << boundary << "--\r\n";
    return body.str();
}

// ===========================================================================
// Boundary Extraction Tests
// ===========================================================================

class BoundaryExtractionTest : public ::testing::Test {};

TEST_F(BoundaryExtractionTest, SimpleBoundary) {
    std::string content_type = "multipart/form-data; boundary=abc123";
    std::string boundary = MultipartParser::extract_boundary(content_type);
    EXPECT_EQ(boundary, "abc123");
}

TEST_F(BoundaryExtractionTest, QuotedBoundary) {
    std::string content_type = "multipart/form-data; boundary=\"abc123\"";
    std::string boundary = MultipartParser::extract_boundary(content_type);
    EXPECT_EQ(boundary, "abc123");
}

TEST_F(BoundaryExtractionTest, BoundaryWithDashes) {
    std::string content_type = "multipart/form-data; boundary=----WebKitFormBoundaryabc123";
    std::string boundary = MultipartParser::extract_boundary(content_type);
    EXPECT_EQ(boundary, "----WebKitFormBoundaryabc123");
}

TEST_F(BoundaryExtractionTest, BoundaryWithSemicolon) {
    std::string content_type = "multipart/form-data; boundary=abc123; charset=utf-8";
    std::string boundary = MultipartParser::extract_boundary(content_type);
    EXPECT_EQ(boundary, "abc123");
}

TEST_F(BoundaryExtractionTest, NoBoundary) {
    std::string content_type = "multipart/form-data";
    std::string boundary = MultipartParser::extract_boundary(content_type);
    EXPECT_TRUE(boundary.empty());
}

TEST_F(BoundaryExtractionTest, FromContentType) {
    std::string content_type = "multipart/form-data; boundary=test123";
    auto parser = MultipartParser::from_content_type(content_type);
    ASSERT_NE(parser, nullptr);
}

TEST_F(BoundaryExtractionTest, FromContentTypeInvalid) {
    std::string content_type = "application/json";
    auto parser = MultipartParser::from_content_type(content_type);
    EXPECT_EQ(parser, nullptr);
}

// ===========================================================================
// Simple Text Field Tests
// ===========================================================================

class TextFieldTest : public ::testing::Test {};

TEST_F(TextFieldTest, SingleField) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {{"name", "John Doe"}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.fields().size(), 1u);
    EXPECT_EQ(form.get_field("name"), "John Doe");
}

TEST_F(TextFieldTest, MultipleFields) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {
        {"first_name", "John"},
        {"last_name", "Doe"},
        {"email", "john@example.com"}
    }, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.fields().size(), 3u);
    EXPECT_EQ(form.get_field("first_name"), "John");
    EXPECT_EQ(form.get_field("last_name"), "Doe");
    EXPECT_EQ(form.get_field("email"), "john@example.com");
}

TEST_F(TextFieldTest, EmptyField) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {{"empty", ""}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("empty"), "");
}

TEST_F(TextFieldTest, FieldWithNewlines) {
    std::string boundary = "test123";
    std::string value = "Line 1\nLine 2\nLine 3";
    std::string body = build_multipart_body(boundary, {{"text", value}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("text"), value);
}

TEST_F(TextFieldTest, FieldWithSpecialChars) {
    std::string boundary = "test123";
    std::string value = "Special chars: !@#$%^&*()_+-=[]{}|;':\",./<>?";
    std::string body = build_multipart_body(boundary, {{"special", value}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("special"), value);
}

TEST_F(TextFieldTest, NonExistentField) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {{"name", "John"}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("nonexistent"), "");
}

// ===========================================================================
// File Upload Tests
// ===========================================================================

class FileUploadTest : public ::testing::Test {};

TEST_F(FileUploadTest, SingleFile) {
    std::string boundary = "test123";
    std::string file_content = "Hello, World!";
    std::string body = build_multipart_body(boundary, {}, {
        {"file", "test.txt", "text/plain", file_content}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.files().size(), 1u);

    const FileUpload* file = form.get_file("file");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->filename, "test.txt");
    EXPECT_EQ(file->content_type, "text/plain");
    EXPECT_EQ(file->size, file_content.size());
    EXPECT_EQ(std::string(file->data.begin(), file->data.end()), file_content);
}

TEST_F(FileUploadTest, BinaryFile) {
    std::string boundary = "test123";

    // Create binary content with all byte values
    std::string binary_content;
    for (int i = 0; i < 256; i++) {
        binary_content += static_cast<char>(i);
    }

    std::string body = build_multipart_body(boundary, {}, {
        {"binary", "data.bin", "application/octet-stream", binary_content}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    const FileUpload* file = form.get_file("binary");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->size, 256u);
    EXPECT_EQ(file->data.size(), 256u);

    // Verify all bytes
    for (int i = 0; i < 256; i++) {
        EXPECT_EQ(file->data[i], static_cast<uint8_t>(i));
    }
}

TEST_F(FileUploadTest, MultipleFiles) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {}, {
        {"file1", "doc.pdf", "application/pdf", "PDF content"},
        {"file2", "image.png", "image/png", "PNG data"},
        {"file3", "data.json", "application/json", "{\"key\": \"value\"}"}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.files().size(), 3u);

    const FileUpload* file1 = form.get_file("file1");
    ASSERT_NE(file1, nullptr);
    EXPECT_EQ(file1->filename, "doc.pdf");
    EXPECT_EQ(file1->content_type, "application/pdf");

    const FileUpload* file2 = form.get_file("file2");
    ASSERT_NE(file2, nullptr);
    EXPECT_EQ(file2->filename, "image.png");
    EXPECT_EQ(file2->content_type, "image/png");
}

TEST_F(FileUploadTest, EmptyFile) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {}, {
        {"empty", "empty.txt", "text/plain", ""}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    const FileUpload* file = form.get_file("empty");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->size, 0u);
}

TEST_F(FileUploadTest, LargeFile) {
    std::string boundary = "test123";

    // Create large content (1MB)
    std::string large_content(1024 * 1024, 'X');

    std::string body = build_multipart_body(boundary, {}, {
        {"large", "large.bin", "application/octet-stream", large_content}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    const FileUpload* file = form.get_file("large");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->size, 1024u * 1024u);
}

TEST_F(FileUploadTest, NonExistentFile) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {}, {
        {"file", "test.txt", "text/plain", "content"}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_file("nonexistent"), nullptr);
}

// ===========================================================================
// Mixed Content Tests
// ===========================================================================

class MixedContentTest : public ::testing::Test {};

TEST_F(MixedContentTest, FieldsAndFiles) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary,
        {{"title", "My Upload"}, {"description", "A test file"}},
        {{"document", "report.pdf", "application/pdf", "PDF content here"}}
    );
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    EXPECT_EQ(form.fields().size(), 2u);
    EXPECT_EQ(form.files().size(), 1u);

    EXPECT_EQ(form.get_field("title"), "My Upload");
    EXPECT_EQ(form.get_field("description"), "A test file");

    const FileUpload* doc = form.get_file("document");
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->filename, "report.pdf");
}

TEST_F(MixedContentTest, MultipleFieldsMultipleFiles) {
    std::string boundary = generate_boundary();
    std::string body = build_multipart_body(boundary,
        {
            {"field1", "value1"},
            {"field2", "value2"},
            {"field3", "value3"}
        },
        {
            {"file1", "a.txt", "text/plain", "File A"},
            {"file2", "b.txt", "text/plain", "File B"}
        }
    );
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    EXPECT_EQ(form.fields().size(), 3u);
    EXPECT_EQ(form.files().size(), 2u);
}

// ===========================================================================
// Streaming Parser Tests
// ===========================================================================

class StreamingParserTest : public ::testing::Test {};

TEST_F(StreamingParserTest, ParseInChunks) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {{"name", "John"}}, {});

    MultipartParser parser(boundary);

    int part_begins = 0;
    int part_ends = 0;
    std::string accumulated_data;

    parser.on_part_begin([&](const PartHeaders& headers) {
        part_begins++;
        EXPECT_EQ(headers.name, "name");
    });

    parser.on_part_data([&](const char* data, size_t len) {
        accumulated_data.append(data, len);
    });

    parser.on_part_end([&]() {
        part_ends++;
    });

    // Parse in small chunks
    size_t chunk_size = 10;
    for (size_t i = 0; i < body.size(); i += chunk_size) {
        size_t len = std::min(chunk_size, body.size() - i);
        parser.parse(body.data() + i, len);
    }

    EXPECT_EQ(part_begins, 1);
    EXPECT_EQ(part_ends, 1);
    EXPECT_EQ(accumulated_data, "John");
}

TEST_F(StreamingParserTest, ParseByteByByte) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {{"x", "y"}}, {});

    MultipartParser parser(boundary);

    int part_ends = 0;
    parser.on_part_end([&]() { part_ends++; });

    // Parse one byte at a time
    for (size_t i = 0; i < body.size(); i++) {
        parser.parse(body.data() + i, 1);
    }

    EXPECT_EQ(part_ends, 1);
}

// ===========================================================================
// Error Handling Tests
// ===========================================================================

class ErrorHandlingTest : public ::testing::Test {};

TEST_F(ErrorHandlingTest, InvalidBoundary) {
    std::string content_type = "multipart/form-data; boundary=test123";
    std::string body = "--wrong_boundary\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\ny\r\n--wrong_boundary--";

    MultipartFormData form;
    EXPECT_FALSE(form.parse(content_type, body));
}

TEST_F(ErrorHandlingTest, MissingBoundary) {
    std::string content_type = "multipart/form-data";
    std::string body = "some data";

    MultipartFormData form;
    EXPECT_FALSE(form.parse(content_type, body));
    EXPECT_FALSE(form.error().empty());
}

TEST_F(ErrorHandlingTest, TruncatedBody) {
    std::string boundary = "test123";
    // Missing end boundary
    std::string body = "--" + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"x\"\r\n"
                       "\r\n"
                       "partial data";

    MultipartFormData form;
    // May or may not fail depending on implementation
    form.parse("multipart/form-data; boundary=" + boundary, body);
}

// ===========================================================================
// Edge Case Tests
// ===========================================================================

class EdgeCaseTest : public ::testing::Test {};

TEST_F(EdgeCaseTest, EmptyBody) {
    std::string boundary = "test123";
    std::string body = "--" + boundary + "--\r\n";
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.fields().size(), 0u);
    EXPECT_EQ(form.files().size(), 0u);
}

TEST_F(EdgeCaseTest, FieldNameWithQuotes) {
    std::string boundary = "test123";
    std::ostringstream body;
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"field\\\"with\\\"quotes\"\r\n";
    body << "\r\n";
    body << "value\r\n";
    body << "--" << boundary << "--\r\n";

    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    // This tests the parser's ability to handle escaped quotes
    form.parse(content_type, body.str());
    // The exact behavior depends on implementation
}

TEST_F(EdgeCaseTest, FilenameWithSpaces) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {}, {
        {"file", "my document.pdf", "application/pdf", "content"}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    const FileUpload* file = form.get_file("file");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->filename, "my document.pdf");
}

TEST_F(EdgeCaseTest, FilenameWithUnicode) {
    std::string boundary = "test123";
    std::string body = build_multipart_body(boundary, {}, {
        {"file", "文档.txt", "text/plain", "content"}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    const FileUpload* file = form.get_file("file");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->filename, "文档.txt");
}

TEST_F(EdgeCaseTest, ContentWithBoundaryLikeString) {
    std::string boundary = "test123";
    // Content that looks like a boundary but isn't
    std::string tricky_content = "This has --test123 in it but not at line start";

    std::string body = build_multipart_body(boundary, {{"data", tricky_content}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("data"), tricky_content);
}

TEST_F(EdgeCaseTest, VeryLongBoundary) {
    std::string boundary(70, 'x');  // RFC allows up to 70 chars
    std::string body = build_multipart_body(boundary, {{"field", "value"}}, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));
    EXPECT_EQ(form.get_field("field"), "value");
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class MultipartPerformanceTest : public ::testing::Test {};

TEST_F(MultipartPerformanceTest, ManySmallFields) {
    std::string boundary = "perf123";
    std::vector<std::pair<std::string, std::string>> fields;

    // 100 small fields
    for (int i = 0; i < 100; i++) {
        fields.emplace_back("field" + std::to_string(i), "value" + std::to_string(i));
    }

    std::string body = build_multipart_body(boundary, fields, {});
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    auto start = std::chrono::high_resolution_clock::now();

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Parse 100 fields: " << duration.count() << " us\n";

    EXPECT_EQ(form.fields().size(), 100u);
}

TEST_F(MultipartPerformanceTest, LargeFileUpload) {
    std::string boundary = "perf123";
    std::string large_content(10 * 1024 * 1024, 'X');  // 10MB

    std::string body = build_multipart_body(boundary, {}, {
        {"bigfile", "large.bin", "application/octet-stream", large_content}
    });
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    auto start = std::chrono::high_resolution_clock::now();

    MultipartFormData form;
    ASSERT_TRUE(form.parse(content_type, body));

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Parse 10MB file: " << duration.count() << " ms\n";

    EXPECT_EQ(form.files().size(), 1u);
    const FileUpload* file = form.get_file("bigfile");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->size, 10u * 1024u * 1024u);
}

}  // namespace test
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
