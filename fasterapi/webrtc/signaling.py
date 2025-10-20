"""
WebRTC Signaling Infrastructure

Manages WebRTC peer connections and message relay.
"""

from typing import Dict, List, Optional, Callable, Any
import json
import time
import uuid


class RTCPeerSession:
    """
    WebRTC peer session.
    
    Represents a connected peer in the signaling server.
    """
    
    def __init__(self, peer_id: str, room_id: str, websocket=None):
        self.id = peer_id
        self.room = room_id
        self.websocket = websocket
        self.state = "connecting"
        self.connected_at = time.time()
        self.last_activity = self.connected_at
    
    def update_activity(self):
        """Update last activity timestamp."""
        self.last_activity = time.time()
    
    def send(self, message: dict):
        """Send message to this peer."""
        if self.websocket:
            self.websocket.send_text(json.dumps(message))


class RTCSignaling:
    """
    WebRTC signaling manager.
    
    Handles SDP exchange and ICE candidate relay for WebRTC connections.
    
    Example:
        signaling = RTCSignaling()
        
        @app.websocket("/rtc/signal")
        def handle_signaling(ws):
            signaling.handle_connection(ws)
    """
    
    def __init__(self):
        self.peers: Dict[str, RTCPeerSession] = {}
        self.rooms: Dict[str, List[str]] = {}
        
        # Statistics
        self.offers_relayed = 0
        self.answers_relayed = 0
        self.ice_candidates_relayed = 0
    
    def register_peer(
        self,
        peer_id: Optional[str] = None,
        room_id: str = "default",
        websocket=None
    ) -> str:
        """
        Register a new peer.
        
        Args:
            peer_id: Unique peer ID (auto-generated if None)
            room_id: Room/channel ID
            websocket: WebSocket connection
            
        Returns:
            Peer ID
        """
        if peer_id is None:
            peer_id = str(uuid.uuid4())
        
        # Create session
        session = RTCPeerSession(peer_id, room_id, websocket)
        self.peers[peer_id] = session
        
        # Add to room
        if room_id not in self.rooms:
            self.rooms[room_id] = []
        self.rooms[room_id].append(peer_id)
        
        return peer_id
    
    def unregister_peer(self, peer_id: str):
        """Unregister a peer."""
        if peer_id not in self.peers:
            return
        
        session = self.peers[peer_id]
        
        # Remove from room
        if session.room in self.rooms:
            self.rooms[session.room] = [
                p for p in self.rooms[session.room] if p != peer_id
            ]
            
            # Remove empty room
            if not self.rooms[session.room]:
                del self.rooms[session.room]
        
        # Remove peer
        del self.peers[peer_id]
    
    def relay_offer(self, from_peer: str, to_peer: str, sdp_offer: str) -> bool:
        """
        Relay SDP offer.
        
        Args:
            from_peer: Source peer ID
            to_peer: Target peer ID
            sdp_offer: SDP offer text
            
        Returns:
            True if relayed successfully
        """
        if to_peer not in self.peers:
            return False
        
        target = self.peers[to_peer]
        target.send({
            "type": "offer",
            "from": from_peer,
            "sdp": sdp_offer
        })
        
        self.offers_relayed += 1
        return True
    
    def relay_answer(self, from_peer: str, to_peer: str, sdp_answer: str) -> bool:
        """Relay SDP answer."""
        if to_peer not in self.peers:
            return False
        
        target = self.peers[to_peer]
        target.send({
            "type": "answer",
            "from": from_peer,
            "sdp": sdp_answer
        })
        
        self.answers_relayed += 1
        return True
    
    def relay_ice_candidate(self, from_peer: str, to_peer: str, candidate: dict) -> bool:
        """Relay ICE candidate."""
        if to_peer not in self.peers:
            return False
        
        target = self.peers[to_peer]
        target.send({
            "type": "ice-candidate",
            "from": from_peer,
            "candidate": candidate
        })
        
        self.ice_candidates_relayed += 1
        return True
    
    def get_room_peers(self, room_id: str) -> List[str]:
        """Get all peers in a room."""
        return self.rooms.get(room_id, [])
    
    def broadcast_to_room(self, room_id: str, message: dict, exclude: Optional[str] = None):
        """
        Broadcast message to all peers in room.
        
        Args:
            room_id: Room ID
            message: Message to broadcast
            exclude: Peer ID to exclude (e.g., sender)
        """
        for peer_id in self.get_room_peers(room_id):
            if peer_id != exclude and peer_id in self.peers:
                self.peers[peer_id].send(message)
    
    def handle_connection(self, ws, room_id: str = "default"):
        """
        Handle WebSocket connection for signaling.
        
        Args:
            ws: WebSocket connection
            room_id: Room to join
        """
        # Register peer
        peer_id = self.register_peer(websocket=ws, room_id=room_id)
        
        try:
            # Send peer ID
            ws.send_json({"type": "registered", "peer_id": peer_id})
            
            # Notify room about new peer
            self.broadcast_to_room(
                room_id,
                {"type": "peer-joined", "peer_id": peer_id},
                exclude=peer_id
            )
            
            # Handle messages
            while ws.is_connected():
                try:
                    msg = ws.receive_json()
                    self.handle_message(peer_id, msg)
                except Exception as e:
                    print(f"Error handling message: {e}")
                    break
        
        finally:
            # Cleanup
            self.unregister_peer(peer_id)
            
            # Notify room
            self.broadcast_to_room(
                room_id,
                {"type": "peer-left", "peer_id": peer_id}
            )
    
    def handle_message(self, peer_id: str, message: dict):
        """Handle signaling message from peer."""
        msg_type = message.get("type")
        target = message.get("target")
        
        if not target:
            return
        
        if msg_type == "offer":
            self.relay_offer(peer_id, target, message.get("sdp", ""))
        
        elif msg_type == "answer":
            self.relay_answer(peer_id, target, message.get("sdp", ""))
        
        elif msg_type == "ice-candidate":
            self.relay_ice_candidate(peer_id, target, message.get("candidate", {}))
    
    def get_stats(self) -> dict:
        """Get signaling statistics."""
        return {
            "total_peers": len(self.peers),
            "active_rooms": len(self.rooms),
            "offers_relayed": self.offers_relayed,
            "answers_relayed": self.answers_relayed,
            "ice_candidates_relayed": self.ice_candidates_relayed
        }

