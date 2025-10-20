#include "signaling.h"
#include <chrono>
#include <iostream>

namespace fasterapi {
namespace webrtc {

RTCSignaling::RTCSignaling() {
}

RTCSignaling::~RTCSignaling() {
    // Cleanup all sessions
    peers_.clear();
    rooms_.clear();
}

int RTCSignaling::register_peer(
    const std::string& peer_id,
    const std::string& room_id,
    void* websocket
) noexcept {
    // Create peer session
    auto session = std::make_unique<RTCPeerSession>(peer_id, room_id);
    session->websocket = websocket;
    session->state = RTCState::CONNECTING;
    session->connected_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    session->last_activity_ns = session->connected_at_ns;
    
    // Add to peers
    peers_[peer_id] = std::move(session);
    
    // Add to room
    rooms_[room_id].insert(peer_id);
    
    std::cout << "WebRTC: Peer " << peer_id << " joined room " << room_id << std::endl;
    
    return 0;
}

int RTCSignaling::unregister_peer(const std::string& peer_id) noexcept {
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return 1;  // Peer not found
    }
    
    // Remove from room
    const std::string& room_id = it->second->room;
    auto room_it = rooms_.find(room_id);
    if (room_it != rooms_.end()) {
        room_it->second.erase(peer_id);
        
        // Remove room if empty
        if (room_it->second.empty()) {
            rooms_.erase(room_it);
        }
    }
    
    // Remove peer
    peers_.erase(it);
    
    std::cout << "WebRTC: Peer " << peer_id << " left" << std::endl;
    
    return 0;
}

int RTCSignaling::relay_offer(
    const std::string& from_peer,
    const std::string& to_peer,
    const std::string& sdp_offer
) noexcept {
    // Build message
    std::string message = R"({"type":"offer","from":")" + from_peer + 
                         R"(","sdp":")" + sdp_offer + R"("})";
    
    int result = send_to_peer(to_peer, message);
    if (result == 0) {
        offers_relayed_++;
    }
    
    return result;
}

int RTCSignaling::relay_answer(
    const std::string& from_peer,
    const std::string& to_peer,
    const std::string& sdp_answer
) noexcept {
    // Build message
    std::string message = R"({"type":"answer","from":")" + from_peer + 
                         R"(","sdp":")" + sdp_answer + R"("})";
    
    int result = send_to_peer(to_peer, message);
    if (result == 0) {
        answers_relayed_++;
    }
    
    return result;
}

int RTCSignaling::relay_ice_candidate(
    const std::string& from_peer,
    const std::string& to_peer,
    const std::string& candidate
) noexcept {
    // Build message
    std::string message = R"({"type":"ice-candidate","from":")" + from_peer + 
                         R"(","candidate":)" + candidate + "}";
    
    int result = send_to_peer(to_peer, message);
    if (result == 0) {
        ice_candidates_relayed_++;
    }
    
    return result;
}

std::vector<std::string> RTCSignaling::get_room_peers(const std::string& room_id) const noexcept {
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return {};
    }
    
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

RTCPeerSession* RTCSignaling::get_peer(const std::string& peer_id) noexcept {
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return nullptr;
    }
    
    return it->second.get();
}

RTCSignaling::Stats RTCSignaling::get_stats() const noexcept {
    Stats stats;
    stats.total_peers = peers_.size();
    stats.active_rooms = rooms_.size();
    stats.offers_relayed = offers_relayed_;
    stats.answers_relayed = answers_relayed_;
    stats.ice_candidates_relayed = ice_candidates_relayed_;
    
    return stats;
}

int RTCSignaling::send_to_peer(const std::string& peer_id, const std::string& message) noexcept {
    auto it = peers_.find(peer_id);
    if (it == peers_.end()) {
        return 1;  // Peer not found
    }
    
    // In real implementation, would send via WebSocket
    // For now, just log
    std::cout << "WebRTC: Relaying to " << peer_id << ": " 
              << message.substr(0, 50) << "..." << std::endl;
    
    // Update last activity
    it->second->last_activity_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    
    return 0;
}

} // namespace webrtc
} // namespace fasterapi

