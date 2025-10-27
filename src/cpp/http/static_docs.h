/**
 * Static Documentation Pages
 *
 * Embedded HTML for Swagger UI and ReDoc.
 * Uses CDN links for assets (no file I/O required).
 *
 * Features:
 * - Swagger UI at /docs
 * - ReDoc at /redoc
 * - Zero file I/O (all HTML embedded)
 * - Configurable OpenAPI spec URL
 */

#pragma once

#include <string>

namespace fasterapi {
namespace http {

/**
 * Static documentation page generator.
 *
 * Generates HTML pages for interactive API documentation.
 */
class StaticDocs {
public:
    /**
     * Generate Swagger UI HTML page.
     *
     * @param openapi_url URL to OpenAPI spec (default: /openapi.json)
     * @param title Page title
     * @return Complete HTML page
     */
    static std::string generate_swagger_ui(
        const std::string& openapi_url = "/openapi.json",
        const std::string& title = "API Documentation"
    ) noexcept;

    /**
     * Generate ReDoc HTML page.
     *
     * @param openapi_url URL to OpenAPI spec (default: /openapi.json)
     * @param title Page title
     * @return Complete HTML page
     */
    static std::string generate_redoc(
        const std::string& openapi_url = "/openapi.json",
        const std::string& title = "API Documentation"
    ) noexcept;

    /**
     * Generate HTTP response for Swagger UI.
     *
     * Includes headers and body.
     *
     * @param openapi_url URL to OpenAPI spec
     * @param title Page title
     * @return Complete HTTP response
     */
    static std::string generate_swagger_ui_response(
        const std::string& openapi_url = "/openapi.json",
        const std::string& title = "API Documentation"
    ) noexcept;

    /**
     * Generate HTTP response for ReDoc.
     *
     * Includes headers and body.
     *
     * @param openapi_url URL to OpenAPI spec
     * @param title Page title
     * @return Complete HTTP response
     */
    static std::string generate_redoc_response(
        const std::string& openapi_url = "/openapi.json",
        const std::string& title = "API Documentation"
    ) noexcept;
};

} // namespace http
} // namespace fasterapi
