/**
 * Static Documentation Pages Implementation
 */

#include "static_docs.h"
#include <sstream>

namespace fasterapi {
namespace http {

std::string StaticDocs::generate_swagger_ui(
    const std::string& openapi_url,
    const std::string& title
) noexcept {
    std::ostringstream html;

    html << R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)" << title << R"(</title>
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-standalone-preset.js"></script>
    <script>
        window.onload = function() {
            SwaggerUIBundle({
                url: ')" << openapi_url << R"(',
                dom_id: '#swagger-ui',
                presets: [
                    SwaggerUIBundle.presets.apis,
                    SwaggerUIStandalonePreset
                ],
                layout: "StandaloneLayout"
            });
        };
    </script>
</body>
</html>)";

    return html.str();
}

std::string StaticDocs::generate_redoc(
    const std::string& openapi_url,
    const std::string& title
) noexcept {
    std::ostringstream html;

    html << R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)" << title << R"(</title>
    <style>
        body {
            margin: 0;
            padding: 0;
        }
    </style>
</head>
<body>
    <redoc spec-url=')" << openapi_url << R"('></redoc>
    <script src="https://cdn.jsdelivr.net/npm/redoc@latest/bundles/redoc.standalone.js"></script>
</body>
</html>)";

    return html.str();
}

std::string StaticDocs::generate_swagger_ui_response(
    const std::string& openapi_url,
    const std::string& title
) noexcept {
    std::string html_body = generate_swagger_ui(openapi_url, title);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/html; charset=utf-8\r\n";
    response << "Content-Length: " << html_body.size() << "\r\n";
    response << "\r\n";
    response << html_body;

    return response.str();
}

std::string StaticDocs::generate_redoc_response(
    const std::string& openapi_url,
    const std::string& title
) noexcept {
    std::string html_body = generate_redoc(openapi_url, title);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/html; charset=utf-8\r\n";
    response << "Content-Length: " << html_body.size() << "\r\n";
    response << "\r\n";
    response << html_body;

    return response.str();
}

} // namespace http
} // namespace fasterapi
