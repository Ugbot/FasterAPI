#!/usr/bin/env python3
"""
WebRTC Signaling and SDP Unit Tests

Tests the WebRTC signaling infrastructure and SDP parsing utilities
without requiring actual WebRTC connections.

Tests cover:
- RTCPeerSession lifecycle
- RTCSignaling peer management
- Room management
- SDP offer/answer relay
- ICE candidate relay
- Broadcasting
- SDP parsing
- SDP generation
- Round-trip SDP parsing/generation
"""

import sys
import time
import random
import string
from unittest.mock import MagicMock, patch

import pytest

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.webrtc.signaling import RTCPeerSession, RTCSignaling
from fasterapi.webrtc.sdp import SDPParser, SDPSession, SDPMedia


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random string."""
    first = random.choice(string.ascii_letters)
    if length == 1:
        return first
    rest = ''.join(random.choices(string.ascii_letters + string.digits, k=length - 1))
    return first + rest


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


def random_peer_id() -> str:
    """Generate a random peer ID."""
    return f"peer-{random_string(8)}"


def random_room_id() -> str:
    """Generate a random room ID."""
    return f"room-{random_string(6)}"


def create_mock_websocket():
    """Create a mock WebSocket for testing."""
    ws = MagicMock()
    ws.send_text = MagicMock()
    ws.send_json = MagicMock()
    ws.receive_json = MagicMock(return_value={})
    ws.is_connected = MagicMock(return_value=True)
    return ws


# Sample SDP offer for testing
SAMPLE_SDP_OFFER = """v=0
o=- 123456789 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:abcd
a=ice-pwd:efghijklmnopqrstuvwxyz
a=sendrecv
m=video 9 UDP/TLS/RTP/SAVPF 96
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:abcd
a=ice-pwd:efghijklmnopqrstuvwxyz
a=sendrecv
"""


# =============================================================================
# RTCPeerSession Tests
# =============================================================================

class TestRTCPeerSession:
    """Tests for RTCPeerSession class."""

    def test_session_creation(self):
        """Test basic session creation."""
        peer_id = random_peer_id()
        room_id = random_room_id()
        session = RTCPeerSession(peer_id, room_id)

        assert session.id == peer_id
        assert session.room == room_id
        assert session.websocket is None
        assert session.state == "connecting"

    def test_session_with_websocket(self):
        """Test session creation with WebSocket."""
        ws = create_mock_websocket()
        session = RTCPeerSession("peer-1", "room-1", websocket=ws)

        assert session.websocket is ws

    def test_session_timestamps(self):
        """Test session timestamps."""
        before = time.time()
        session = RTCPeerSession("peer-1", "room-1")
        after = time.time()

        assert before <= session.connected_at <= after
        assert session.last_activity == session.connected_at

    def test_update_activity(self):
        """Test updating activity timestamp."""
        session = RTCPeerSession("peer-1", "room-1")
        original_activity = session.last_activity

        time.sleep(0.01)  # Small delay
        session.update_activity()

        assert session.last_activity > original_activity

    def test_send_with_websocket(self):
        """Test sending message through WebSocket."""
        ws = create_mock_websocket()
        session = RTCPeerSession("peer-1", "room-1", websocket=ws)

        message = {"type": "test", "data": random_string(20)}
        session.send(message)

        ws.send_text.assert_called_once()
        # Verify JSON was sent
        call_args = ws.send_text.call_args[0][0]
        assert "type" in call_args
        assert "test" in call_args

    def test_send_without_websocket(self):
        """Test sending message without WebSocket doesn't crash."""
        session = RTCPeerSession("peer-1", "room-1")
        session.send({"type": "test"})  # Should not raise


# =============================================================================
# RTCSignaling Tests
# =============================================================================

class TestRTCSignaling:
    """Tests for RTCSignaling class."""

    def test_signaling_creation(self):
        """Test signaling manager creation."""
        signaling = RTCSignaling()

        assert signaling.peers == {}
        assert signaling.rooms == {}
        assert signaling.offers_relayed == 0
        assert signaling.answers_relayed == 0
        assert signaling.ice_candidates_relayed == 0

    def test_register_peer_auto_id(self):
        """Test registering peer with auto-generated ID."""
        signaling = RTCSignaling()
        peer_id = signaling.register_peer()

        assert peer_id is not None
        assert len(peer_id) > 0
        assert peer_id in signaling.peers

    def test_register_peer_custom_id(self):
        """Test registering peer with custom ID."""
        signaling = RTCSignaling()
        custom_id = random_peer_id()
        peer_id = signaling.register_peer(peer_id=custom_id)

        assert peer_id == custom_id
        assert custom_id in signaling.peers

    def test_register_peer_with_room(self):
        """Test registering peer in specific room."""
        signaling = RTCSignaling()
        room_id = random_room_id()
        peer_id = signaling.register_peer(room_id=room_id)

        assert room_id in signaling.rooms
        assert peer_id in signaling.rooms[room_id]

    def test_register_peer_default_room(self):
        """Test registering peer goes to default room."""
        signaling = RTCSignaling()
        peer_id = signaling.register_peer()

        assert "default" in signaling.rooms
        assert peer_id in signaling.rooms["default"]

    def test_register_multiple_peers_same_room(self):
        """Test registering multiple peers in same room."""
        signaling = RTCSignaling()
        room_id = random_room_id()

        peer_ids = [signaling.register_peer(room_id=room_id) for _ in range(5)]

        assert len(signaling.rooms[room_id]) == 5
        for pid in peer_ids:
            assert pid in signaling.rooms[room_id]

    def test_unregister_peer(self):
        """Test unregistering a peer."""
        signaling = RTCSignaling()
        peer_id = signaling.register_peer()

        signaling.unregister_peer(peer_id)

        assert peer_id not in signaling.peers

    def test_unregister_peer_removes_from_room(self):
        """Test unregistering peer removes from room."""
        signaling = RTCSignaling()
        room_id = random_room_id()
        peer_id = signaling.register_peer(room_id=room_id)

        signaling.unregister_peer(peer_id)

        if room_id in signaling.rooms:
            assert peer_id not in signaling.rooms[room_id]

    def test_unregister_last_peer_removes_room(self):
        """Test unregistering last peer removes empty room."""
        signaling = RTCSignaling()
        room_id = random_room_id()
        peer_id = signaling.register_peer(room_id=room_id)

        signaling.unregister_peer(peer_id)

        assert room_id not in signaling.rooms

    def test_unregister_nonexistent_peer(self):
        """Test unregistering non-existent peer doesn't crash."""
        signaling = RTCSignaling()
        signaling.unregister_peer("nonexistent")  # Should not raise

    def test_get_room_peers(self):
        """Test getting peers in a room."""
        signaling = RTCSignaling()
        room_id = random_room_id()

        peer_ids = [signaling.register_peer(room_id=room_id) for _ in range(3)]
        room_peers = signaling.get_room_peers(room_id)

        assert len(room_peers) == 3
        for pid in peer_ids:
            assert pid in room_peers

    def test_get_room_peers_empty(self):
        """Test getting peers from non-existent room."""
        signaling = RTCSignaling()
        peers = signaling.get_room_peers("nonexistent")

        assert peers == []


# =============================================================================
# RTCSignaling Relay Tests
# =============================================================================

class TestRTCSignalingRelay:
    """Tests for RTCSignaling relay functionality."""

    def test_relay_offer_success(self):
        """Test relaying SDP offer successfully."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        result = signaling.relay_offer(peer1, peer2, SAMPLE_SDP_OFFER)

        assert result is True
        assert signaling.offers_relayed == 1
        ws.send_text.assert_called_once()

    def test_relay_offer_target_not_found(self):
        """Test relaying offer to non-existent peer."""
        signaling = RTCSignaling()
        peer1 = signaling.register_peer()

        result = signaling.relay_offer(peer1, "nonexistent", SAMPLE_SDP_OFFER)

        assert result is False
        assert signaling.offers_relayed == 0

    def test_relay_answer_success(self):
        """Test relaying SDP answer successfully."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        result = signaling.relay_answer(peer1, peer2, "v=0...")

        assert result is True
        assert signaling.answers_relayed == 1
        ws.send_text.assert_called_once()

    def test_relay_answer_target_not_found(self):
        """Test relaying answer to non-existent peer."""
        signaling = RTCSignaling()
        peer1 = signaling.register_peer()

        result = signaling.relay_answer(peer1, "nonexistent", "v=0...")

        assert result is False
        assert signaling.answers_relayed == 0

    def test_relay_ice_candidate_success(self):
        """Test relaying ICE candidate successfully."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        candidate = {
            "candidate": "candidate:1 1 UDP 2130706431 192.168.1.1 54321 typ host",
            "sdpMid": "0",
            "sdpMLineIndex": 0
        }

        result = signaling.relay_ice_candidate(peer1, peer2, candidate)

        assert result is True
        assert signaling.ice_candidates_relayed == 1

    def test_relay_ice_candidate_target_not_found(self):
        """Test relaying ICE candidate to non-existent peer."""
        signaling = RTCSignaling()
        peer1 = signaling.register_peer()

        result = signaling.relay_ice_candidate(peer1, "nonexistent", {})

        assert result is False
        assert signaling.ice_candidates_relayed == 0


# =============================================================================
# RTCSignaling Broadcast Tests
# =============================================================================

class TestRTCSignalingBroadcast:
    """Tests for RTCSignaling broadcast functionality."""

    def test_broadcast_to_room(self):
        """Test broadcasting to all peers in room."""
        signaling = RTCSignaling()
        room_id = random_room_id()

        ws_list = [create_mock_websocket() for _ in range(3)]
        peer_ids = [
            signaling.register_peer(room_id=room_id, websocket=ws)
            for ws in ws_list
        ]

        message = {"type": "announcement", "data": random_string(20)}
        signaling.broadcast_to_room(room_id, message)

        for ws in ws_list:
            ws.send_text.assert_called_once()

    def test_broadcast_excludes_sender(self):
        """Test broadcast excludes specified peer."""
        signaling = RTCSignaling()
        room_id = random_room_id()

        ws1 = create_mock_websocket()
        ws2 = create_mock_websocket()

        peer1 = signaling.register_peer(room_id=room_id, websocket=ws1)
        peer2 = signaling.register_peer(room_id=room_id, websocket=ws2)

        signaling.broadcast_to_room(room_id, {"type": "test"}, exclude=peer1)

        ws1.send_text.assert_not_called()
        ws2.send_text.assert_called_once()

    def test_broadcast_empty_room(self):
        """Test broadcasting to empty room doesn't crash."""
        signaling = RTCSignaling()
        signaling.broadcast_to_room("nonexistent", {"type": "test"})


# =============================================================================
# RTCSignaling Message Handling Tests
# =============================================================================

class TestRTCSignalingMessageHandling:
    """Tests for RTCSignaling message handling."""

    def test_handle_offer_message(self):
        """Test handling offer message."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        message = {
            "type": "offer",
            "target": peer2,
            "sdp": SAMPLE_SDP_OFFER
        }
        signaling.handle_message(peer1, message)

        assert signaling.offers_relayed == 1

    def test_handle_answer_message(self):
        """Test handling answer message."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        message = {
            "type": "answer",
            "target": peer2,
            "sdp": "v=0..."
        }
        signaling.handle_message(peer1, message)

        assert signaling.answers_relayed == 1

    def test_handle_ice_candidate_message(self):
        """Test handling ICE candidate message."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        message = {
            "type": "ice-candidate",
            "target": peer2,
            "candidate": {"candidate": "..."}
        }
        signaling.handle_message(peer1, message)

        assert signaling.ice_candidates_relayed == 1

    def test_handle_message_no_target(self):
        """Test handling message without target is ignored."""
        signaling = RTCSignaling()
        peer = signaling.register_peer()

        signaling.handle_message(peer, {"type": "offer", "sdp": "..."})

        assert signaling.offers_relayed == 0


# =============================================================================
# RTCSignaling Stats Tests
# =============================================================================

class TestRTCSignalingStats:
    """Tests for RTCSignaling statistics."""

    def test_get_stats_empty(self):
        """Test getting stats from empty signaling."""
        signaling = RTCSignaling()
        stats = signaling.get_stats()

        assert stats['total_peers'] == 0
        assert stats['active_rooms'] == 0
        assert stats['offers_relayed'] == 0
        assert stats['answers_relayed'] == 0
        assert stats['ice_candidates_relayed'] == 0

    def test_get_stats_with_peers(self):
        """Test getting stats with peers."""
        signaling = RTCSignaling()

        for _ in range(5):
            signaling.register_peer(room_id="room1")
        for _ in range(3):
            signaling.register_peer(room_id="room2")

        stats = signaling.get_stats()

        assert stats['total_peers'] == 8
        assert stats['active_rooms'] == 2

    def test_get_stats_with_activity(self):
        """Test getting stats after relay activity."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        signaling.relay_offer(peer1, peer2, "sdp")
        signaling.relay_answer(peer2, peer1, "sdp")
        signaling.relay_ice_candidate(peer1, peer2, {})
        signaling.relay_ice_candidate(peer1, peer2, {})

        stats = signaling.get_stats()

        assert stats['offers_relayed'] == 1
        assert stats['answers_relayed'] == 1
        assert stats['ice_candidates_relayed'] == 2


# =============================================================================
# SDP Parser Tests
# =============================================================================

class TestSDPParser:
    """Tests for SDP parser."""

    def test_parse_basic_sdp(self):
        """Test parsing basic SDP."""
        sdp = """v=0
o=- 123456 2 IN IP4 127.0.0.1
s=-
t=0 0
"""
        session = SDPParser.parse(sdp)

        assert session.version == "0"
        assert "123456" in session.origin
        assert session.session_name == "-"
        assert session.timing == "0 0"

    def test_parse_sdp_with_media(self):
        """Test parsing SDP with media sections."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)

        assert len(session.media) == 2
        assert session.media[0].media_type == "audio"
        assert session.media[1].media_type == "video"

    def test_parse_media_attributes(self):
        """Test parsing media attributes."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)

        audio = session.media[0]
        assert "ice-ufrag" in audio.attributes
        assert audio.attributes["ice-ufrag"] == "abcd"

    def test_parse_session_attributes(self):
        """Test parsing session-level attributes."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)

        assert "group" in session.attributes

    def test_parse_media_port(self):
        """Test parsing media port."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)

        assert session.media[0].port == 9
        assert session.media[1].port == 9

    def test_parse_media_protocol(self):
        """Test parsing media protocol."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)

        assert session.media[0].protocol == "UDP/TLS/RTP/SAVPF"

    def test_parse_connection(self):
        """Test parsing connection info."""
        sdp = """v=0
o=- 123 2 IN IP4 127.0.0.1
s=-
c=IN IP4 192.168.1.1
t=0 0
"""
        session = SDPParser.parse(sdp)
        assert session.connection == "IN IP4 192.168.1.1"

    def test_parse_empty_sdp(self):
        """Test parsing empty SDP."""
        session = SDPParser.parse("")
        assert session.version == "0"  # Default
        assert session.media == []

    def test_parse_malformed_lines_ignored(self):
        """Test malformed lines are ignored."""
        sdp = """v=0
this is not valid
o=- 123 2 IN IP4 127.0.0.1
s=-
t=0 0
"""
        session = SDPParser.parse(sdp)
        assert session.version == "0"


# =============================================================================
# SDP Generator Tests
# =============================================================================

class TestSDPGenerator:
    """Tests for SDP generator."""

    def test_generate_basic_sdp(self):
        """Test generating basic SDP."""
        session = SDPSession(
            version="0",
            origin="- 123456 2 IN IP4 127.0.0.1",
            session_name="Test Session"
        )

        sdp = SDPParser.generate(session)

        assert "v=0" in sdp
        assert "o=- 123456" in sdp
        assert "s=Test Session" in sdp

    def test_generate_sdp_with_media(self):
        """Test generating SDP with media."""
        session = SDPSession()
        session.media.append(SDPMedia(
            media_type="audio",
            port=9,
            protocol="UDP/TLS/RTP/SAVPF",
            formats=["111"]
        ))

        sdp = SDPParser.generate(session)

        assert "m=audio 9 UDP/TLS/RTP/SAVPF 111" in sdp

    def test_generate_sdp_with_attributes(self):
        """Test generating SDP with attributes."""
        session = SDPSession()
        session.attributes["group"] = "BUNDLE 0 1"

        sdp = SDPParser.generate(session)

        assert "a=group:BUNDLE 0 1" in sdp

    def test_generate_sdp_with_flag_attribute(self):
        """Test generating SDP with flag (no value) attribute."""
        session = SDPSession()
        session.media.append(SDPMedia(
            media_type="audio",
            port=9,
            protocol="RTP/SAVPF"
        ))
        session.media[0].attributes["sendrecv"] = ""

        sdp = SDPParser.generate(session)

        assert "a=sendrecv" in sdp

    def test_generate_sdp_crlf_endings(self):
        """Test generated SDP uses CRLF line endings."""
        session = SDPSession()
        sdp = SDPParser.generate(session)

        assert "\r\n" in sdp


# =============================================================================
# SDP Round-Trip Tests
# =============================================================================

class TestSDPRoundTrip:
    """Tests for SDP parsing and regeneration."""

    def test_round_trip_basic(self):
        """Test parsing and regenerating basic SDP."""
        original = """v=0
o=- 123456 2 IN IP4 127.0.0.1
s=-
t=0 0
"""
        session = SDPParser.parse(original)
        regenerated = SDPParser.generate(session)

        assert "v=0" in regenerated
        assert "o=- 123456" in regenerated

    def test_round_trip_preserves_media(self):
        """Test round trip preserves media sections."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)
        regenerated = SDPParser.generate(session)

        assert "m=audio" in regenerated
        assert "m=video" in regenerated

    def test_round_trip_preserves_attributes(self):
        """Test round trip preserves attributes."""
        session = SDPParser.parse(SAMPLE_SDP_OFFER)
        regenerated = SDPParser.generate(session)

        assert "a=ice-ufrag:abcd" in regenerated
        assert "a=sendrecv" in regenerated


# =============================================================================
# SDPSession/SDPMedia Dataclass Tests
# =============================================================================

class TestSDPDataclasses:
    """Tests for SDP dataclasses."""

    def test_sdp_session_defaults(self):
        """Test SDPSession default values."""
        session = SDPSession()

        assert session.version == "0"
        assert session.origin == ""
        assert session.session_name == "-"
        assert session.timing == "0 0"
        assert session.media == []
        assert session.attributes == {}

    def test_sdp_media_defaults(self):
        """Test SDPMedia default values."""
        media = SDPMedia(media_type="audio", port=9, protocol="RTP")

        assert media.media_type == "audio"
        assert media.port == 9
        assert media.protocol == "RTP"
        assert media.formats == []
        assert media.attributes == {}

    def test_sdp_media_with_formats(self):
        """Test SDPMedia with formats."""
        media = SDPMedia(
            media_type="video",
            port=9,
            protocol="RTP/SAVPF",
            formats=["96", "97", "98"]
        )

        assert len(media.formats) == 3
        assert "96" in media.formats

    def test_sdp_session_mutable(self):
        """Test SDPSession is mutable."""
        session = SDPSession()
        session.version = "1"
        session.attributes["custom"] = "value"
        session.media.append(SDPMedia("audio", 9, "RTP"))

        assert session.version == "1"
        assert session.attributes["custom"] == "value"
        assert len(session.media) == 1


# =============================================================================
# Stress Tests
# =============================================================================

class TestStress:
    """Stress tests for WebRTC components."""

    def test_many_peers(self):
        """Test with many peers."""
        signaling = RTCSignaling()
        room_id = random_room_id()

        peer_ids = [signaling.register_peer(room_id=room_id) for _ in range(100)]

        assert len(signaling.peers) == 100
        assert len(signaling.rooms[room_id]) == 100

        # Unregister half
        for pid in peer_ids[:50]:
            signaling.unregister_peer(pid)

        assert len(signaling.peers) == 50

    def test_many_rooms(self):
        """Test with many rooms."""
        signaling = RTCSignaling()

        for _ in range(50):
            room_id = random_room_id()
            for _ in range(3):
                signaling.register_peer(room_id=room_id)

        assert len(signaling.rooms) == 50
        stats = signaling.get_stats()
        assert stats['total_peers'] == 150
        assert stats['active_rooms'] == 50

    def test_many_relays(self):
        """Test many relay operations."""
        signaling = RTCSignaling()
        ws = create_mock_websocket()

        peer1 = signaling.register_peer()
        peer2 = signaling.register_peer(websocket=ws)

        for _ in range(100):
            signaling.relay_offer(peer1, peer2, "sdp")
            signaling.relay_answer(peer2, peer1, "sdp")
            signaling.relay_ice_candidate(peer1, peer2, {})

        stats = signaling.get_stats()
        assert stats['offers_relayed'] == 100
        assert stats['answers_relayed'] == 100
        assert stats['ice_candidates_relayed'] == 100

    def test_complex_sdp_parsing(self):
        """Test parsing complex SDP with many media sections."""
        # Generate complex SDP
        lines = [
            "v=0",
            "o=- 123456789 2 IN IP4 127.0.0.1",
            "s=Complex Session",
            "t=0 0",
            "a=group:BUNDLE " + " ".join(str(i) for i in range(10))
        ]

        for i in range(10):
            lines.extend([
                f"m=video 9 UDP/TLS/RTP/SAVPF 96",
                f"a=mid:{i}",
                f"a=ice-ufrag:ufrag{i}",
                f"a=ice-pwd:pwd{i}",
                f"a=sendrecv"
            ])

        sdp = "\n".join(lines)
        session = SDPParser.parse(sdp)

        assert len(session.media) == 10


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
