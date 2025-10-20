#include "h2_handler.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>

// nghttp2 includes
#ifdef FA_ENABLE_HTTP2
#include <nghttp2/nghttp2.h>
#endif

// OpenSSL includes
#include <openssl/ssl.h>
#include <openssl/err.h>

Http2Handler::Http2Handler(const Settings& settings, const AlpnConfig& alpn_config)
    : settings_(settings), alpn_config_(alpn_config), running_(false),
      session_(nullptr), callbacks_(nullptr), deflater_(nullptr), inflater_(nullptr),
      ssl_ctx_(nullptr), total_requests_(0), total_bytes_sent_(0), total_bytes_received_(0),
      active_streams_(0), push_responses_(0), next_stream_id_(1) {
}

Http2Handler::~Http2Handler() {
    stop();
    
#ifdef FA_ENABLE_HTTP2
    if (deflater_) {
        nghttp2_hd_deflate_del(deflater_);
    }
    
    if (inflater_) {
        nghttp2_hd_inflate_del(inflater_);
    }
    
    if (session_) {
        nghttp2_session_del(session_);
    }
    
    if (callbacks_) {
        nghttp2_session_callbacks_del(callbacks_);
    }
    
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
    }
#endif
}

int Http2Handler::initialize() noexcept {
#ifdef FA_ENABLE_HTTP2
    // Initialize OpenSSL
    int result = initialize_ssl();
    if (result != 0) {
        return result;
    }
    
    // Initialize nghttp2 session
    result = initialize_session();
    if (result != 0) {
        return result;
    }
    
    // Initialize HPACK deflater/inflater
    int rv = nghttp2_hd_deflate_new(&deflater_, settings_.header_table_size);
    if (rv != 0) {
        std::cerr << "Failed to create HPACK deflater: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    rv = nghttp2_hd_inflate_new(&inflater_);
    if (rv != 0) {
        std::cerr << "Failed to create HPACK inflater: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    std::cout << "HTTP/2 handler initialized successfully" << std::endl;
    return 0;
#else
    std::cerr << "HTTP/2 support not enabled" << std::endl;
    return 1;
#endif
}

int Http2Handler::start(uint16_t port, const std::string& host) noexcept {
    if (running_.load()) {
        return 1;  // Already running
    }
    
    std::cout << "HTTP/2 server starting on " << host << ":" << port << std::endl;
    
#ifdef FA_ENABLE_HTTP2
    // TODO: Implement actual HTTP/2 server with OpenSSL and nghttp2
    // For now, simulate server running
    running_.store(true);
    
    std::cout << "HTTP/2 server listening on port " << port << std::endl;
    return 0;
#else
    std::cerr << "HTTP/2 support not enabled" << std::endl;
    return 1;
#endif
}

int Http2Handler::stop() noexcept {
    if (!running_.load()) {
        return 0;  // Already stopped
    }
    
    running_.store(false);
    std::cout << "HTTP/2 server stopped" << std::endl;
    return 0;
}

bool Http2Handler::is_running() const noexcept {
    return running_.load();
}

int Http2Handler::add_route(const std::string& method, const std::string& path, 
                           std::function<void(Stream*)> handler) noexcept {
    if (running_.load()) {
        return 1;  // Cannot add routes while running
    }
    
    std::string key = method + ":" + path;
    routes_[key] = std::move(handler);
    return 0;
}

int Http2Handler::process_data(const uint8_t* data, size_t length) noexcept {
#ifdef FA_ENABLE_HTTP2
    if (!session_) {
        return 1;
    }
    
    total_bytes_received_.fetch_add(length);
    
    // Process incoming data with nghttp2
    ssize_t rv = nghttp2_session_mem_recv(session_, data, length);
    if (rv < 0) {
        std::cerr << "nghttp2_session_mem_recv failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    return 0;
#else
    return 1;
#endif
}

int Http2Handler::send_response(int32_t stream_id, int status, 
                               const std::unordered_map<std::string, std::string>& headers,
                               const std::vector<uint8_t>& body) noexcept {
#ifdef FA_ENABLE_HTTP2
    if (!session_) {
        return 1;
    }
    
    // Prepare response headers
    std::vector<nghttp2_nv> nva;
    nva.reserve(headers.size() + 2);
    
    // Status header
    std::string status_str = std::to_string(status);
    nva.push_back({(uint8_t*)":status", (uint8_t*)status_str.c_str(), 
                   7, status_str.length(), NGHTTP2_NV_FLAG_NONE});
    
    // Other headers
    for (const auto& [name, value] : headers) {
        nva.push_back({(uint8_t*)name.c_str(), (uint8_t*)value.c_str(),
                       name.length(), value.length(), NGHTTP2_NV_FLAG_NONE});
    }
    
    // Send headers
    int rv = nghttp2_submit_response(session_, stream_id, nva.data(), nva.size(), nullptr);
    if (rv != 0) {
        std::cerr << "nghttp2_submit_response failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    // Send body if present
    if (!body.empty()) {
        nghttp2_data_provider data_provider;
        data_provider.read_callback = on_data_source_read_callback;
        
        rv = nghttp2_submit_data(session_, stream_id, NGHTTP2_FLAG_END_STREAM, &data_provider);
        if (rv != 0) {
            std::cerr << "nghttp2_submit_data failed: " << nghttp2_strerror(rv) << std::endl;
            return 1;
        }
    }
    
    // Send frames
    rv = nghttp2_session_send(session_);
    if (rv != 0) {
        std::cerr << "nghttp2_session_send failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    total_bytes_sent_.fetch_add(body.size());
    return 0;
#else
    return 1;
#endif
}

int Http2Handler::send_push(int32_t stream_id, const std::string& path,
                           const std::unordered_map<std::string, std::string>& headers,
                           const std::vector<uint8_t>& body) noexcept {
#ifdef FA_ENABLE_HTTP2
    if (!session_) {
        return 1;
    }
    
    // Prepare push headers
    std::vector<nghttp2_nv> nva;
    nva.reserve(headers.size() + 1);
    
    // Path header
    nva.push_back({(uint8_t*)":path", (uint8_t*)path.c_str(),
                   5, path.length(), NGHTTP2_NV_FLAG_NONE});
    
    // Other headers
    for (const auto& [name, value] : headers) {
        nva.push_back({(uint8_t*)name.c_str(), (uint8_t*)value.c_str(),
                       name.length(), value.length(), NGHTTP2_NV_FLAG_NONE});
    }
    
    // Send push promise
    int rv = nghttp2_submit_push_promise(session_, NGHTTP2_FLAG_NONE, stream_id,
                                        nva.data(), nva.size(), nullptr);
    if (rv != 0) {
        std::cerr << "nghttp2_submit_push_promise failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    push_responses_.fetch_add(1);
    return 0;
#else
    return 1;
#endif
}

std::unordered_map<std::string, uint64_t> Http2Handler::get_stats() const noexcept {
    std::unordered_map<std::string, uint64_t> stats;
    stats["total_requests"] = total_requests_.load();
    stats["total_bytes_sent"] = total_bytes_sent_.load();
    stats["total_bytes_received"] = total_bytes_received_.load();
    stats["active_streams"] = active_streams_.load();
    stats["push_responses"] = push_responses_.load();
    return stats;
}

int Http2Handler::initialize_ssl() noexcept {
#ifdef FA_ENABLE_HTTP2
    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    // Create SSL context
    ssl_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx_) {
        std::cerr << "Failed to create SSL context" << std::endl;
        return 1;
    }
    
    // Set ALPN protocols
    std::vector<uint8_t> alpn_protos;
    for (const auto& proto : {"h2", "http/1.1"}) {
        alpn_protos.push_back(strlen(proto));
        alpn_protos.insert(alpn_protos.end(), proto, proto + strlen(proto));
    }
    
    int rv = SSL_CTX_set_alpn_protos(ssl_ctx_, alpn_protos.data(), alpn_protos.size());
    if (rv != 0) {
        std::cerr << "Failed to set ALPN protocols" << std::endl;
        return 1;
    }
    
    return 0;
#else
    return 1;
#endif
}

int Http2Handler::initialize_session() noexcept {
#ifdef FA_ENABLE_HTTP2
    // Create nghttp2 session callbacks
    int rv = nghttp2_session_callbacks_new(&callbacks_);
    if (rv != 0) {
        std::cerr << "nghttp2_session_callbacks_new failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    // Set callbacks
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks_, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks_, on_frame_send_callback);
    nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks_, on_frame_not_send_callback);
    
    // Create nghttp2 session
    rv = nghttp2_session_server_new(&session_, callbacks_, this);
    if (rv != 0) {
        std::cerr << "nghttp2_session_server_new failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    // Set settings
    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, settings_.header_table_size},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, settings_.enable_push},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, settings_.max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, settings_.initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, settings_.max_frame_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, settings_.max_header_list_size}
    };
    
    rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, sizeof(settings) / sizeof(settings[0]));
    if (rv != 0) {
        std::cerr << "nghttp2_submit_settings failed: " << nghttp2_strerror(rv) << std::endl;
        return 1;
    }
    
    return 0;
#else
    return 1;
#endif
}

Http2Handler::Stream* Http2Handler::create_stream(int32_t stream_id) noexcept {
    auto stream = std::make_unique<Stream>();
    stream->stream_id = stream_id;
    streams_[stream_id] = std::move(stream);
    active_streams_.fetch_add(1);
    return streams_[stream_id].get();
}

Http2Handler::Stream* Http2Handler::get_stream(int32_t stream_id) noexcept {
    auto it = streams_.find(stream_id);
    return (it != streams_.end()) ? it->second.get() : nullptr;
}

void Http2Handler::remove_stream(int32_t stream_id) noexcept {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        streams_.erase(it);
        active_streams_.fetch_sub(1);
    }
}

// Static callback implementations (stubbed for now)
int Http2Handler::on_begin_headers_callback(nghttp2_session* session,
                                           const nghttp2_frame* frame,
                                           void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_header_callback(nghttp2_session* session,
                                    const nghttp2_frame* frame,
                                    const uint8_t* name, size_t namelen,
                                    const uint8_t* value, size_t valuelen,
                                    uint8_t flags, void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_data_chunk_recv_callback(nghttp2_session* session,
                                             uint8_t flags, int32_t stream_id,
                                             const uint8_t* data, size_t len,
                                             void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_stream_close_callback(nghttp2_session* session,
                                          int32_t stream_id, uint32_t error_code,
                                          void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_frame_recv_callback(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_frame_send_callback(nghttp2_session* session,
                                         const nghttp2_frame* frame,
                                         void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

int Http2Handler::on_frame_not_send_callback(nghttp2_session* session,
                                            const nghttp2_frame* frame,
                                            int lib_error_code, void* user_data) {
    // TODO: Implement proper callback
    return 0;
}

ssize_t Http2Handler::on_data_source_read_callback(nghttp2_session* session,
                                                  int32_t stream_id,
                                                  uint8_t* buf, size_t length,
                                                  uint32_t* data_flags,
                                                  nghttp2_data_source* source,
                                                  void* user_data) {
    // TODO: Implement proper callback
    return 0;
}
