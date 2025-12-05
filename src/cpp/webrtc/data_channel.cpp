#include "data_channel.h"
#include <iostream>
#include <cstring>

namespace fasterapi {
namespace webrtc {

DataChannel::DataChannel(const std::string& label, const DataChannelOptions& options)
    : label_(label), options_(options), state_(DataChannelState::CONNECTING) {
}

DataChannel::~DataChannel() {
    close();
}

int DataChannel::send_text(std::string_view data) noexcept {
    if (state_ != DataChannelState::OPEN) {
        return 1;  // Channel not open
    }
    
    // Send as text (SCTP PPID 51 for WebRTC)
    int result = send_sctp(
        reinterpret_cast<const uint8_t*>(data.data()),
        data.length(),
        false  // text
    );
    
    if (result == 0) {
        messages_sent_++;
        bytes_sent_ += data.length();
    }
    
    return result;
}

int DataChannel::send_binary(const uint8_t* data, size_t len) noexcept {
    if (state_ != DataChannelState::OPEN) {
        return 1;
    }
    
    // Send as binary (SCTP PPID 53 for WebRTC)
    int result = send_sctp(data, len, true);
    
    if (result == 0) {
        messages_sent_++;
        bytes_sent_ += len;
    }
    
    return result;
}

void DataChannel::on_message(MessageHandler handler) {
    message_handler_ = std::move(handler);
}

void DataChannel::on_state_change(StateHandler handler) {
    state_handler_ = std::move(handler);
}

int DataChannel::close() noexcept {
    if (state_ == DataChannelState::CLOSED) {
        return 0;
    }
    
    state_ = DataChannelState::CLOSING;
    
    // TODO: Send SCTP close
    
    state_ = DataChannelState::CLOSED;
    
    if (state_handler_) {
        state_handler_(state_);
    }
    
    return 0;
}

DataChannel::Stats DataChannel::get_stats() const noexcept {
    Stats stats;
    stats.messages_sent = messages_sent_;
    stats.messages_received = messages_received_;
    stats.bytes_sent = bytes_sent_;
    stats.bytes_received = bytes_received_;
    return stats;
}

int DataChannel::process_sctp_data(const uint8_t* data, size_t len) noexcept {
    if (!data || len == 0) {
        return 1;
    }

    // Default to text - this is used when PPID not available
    return receive_data(data, len, SCTPPayloadProtocolId::WEBRTC_STRING);
}

int DataChannel::receive_data(const uint8_t* data, size_t len, SCTPPayloadProtocolId ppid) noexcept {
    // Handle empty messages (RFC 8831)
    // Empty PPIDs (54, 56) allow null/empty data
    if (len == 0) {
        if (ppid != SCTPPayloadProtocolId::WEBRTC_STRING_EMPTY &&
            ppid != SCTPPayloadProtocolId::WEBRTC_BINARY_EMPTY) {
            return 1;  // Empty data must use empty PPIDs
        }
    } else if (!data) {
        // Non-empty messages require data pointer
        return 1;
    }

    messages_received_++;
    bytes_received_ += len;

    // Determine if binary based on PPID
    bool is_binary = false;
    switch (ppid) {
        case SCTPPayloadProtocolId::WEBRTC_BINARY:
        case SCTPPayloadProtocolId::WEBRTC_BINARY_EMPTY:
        case SCTPPayloadProtocolId::WEBRTC_BINARY_PARTIAL:
        case SCTPPayloadProtocolId::WEBRTC_BINARY_PARTIAL2:
            is_binary = true;
            break;
        case SCTPPayloadProtocolId::WEBRTC_STRING:
        case SCTPPayloadProtocolId::WEBRTC_STRING_EMPTY:
        case SCTPPayloadProtocolId::WEBRTC_DCEP:
        default:
            is_binary = false;
            break;
    }

    // Call message handler
    if (message_handler_) {
        // Handle null data for empty messages (string_view with nullptr is UB)
        static const char empty_data[] = "";
        const char* msg_data = data ? reinterpret_cast<const char*>(data) : empty_data;

        DataChannelMessage msg(
            std::string_view(msg_data, len),
            is_binary
        );
        message_handler_(msg);
    }

    return 0;
}

int DataChannel::send_sctp(const uint8_t* data, size_t len, bool binary) noexcept {
    // TODO: Build SCTP DATA chunk
    // TODO: Send via DTLS
    
    // For now, simplified - just simulate send
    std::cout << "DataChannel: Sending " << len << " bytes ("
              << (binary ? "binary" : "text") << ")" << std::endl;
    
    return 0;
}

} // namespace webrtc
} // namespace fasterapi

