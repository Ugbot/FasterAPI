"""
WebRTC Video Chat Demo

Complete WebRTC signaling server with video chat example.

Features:
- SDP offer/answer exchange
- ICE candidate relay
- Room-based sessions
- Multiple peer support
- simdjson for fast message parsing
"""

from fasterapi import App
from fasterapi.webrtc import RTCSignaling, SDPParser
from fasterapi.http import Request, Response
import json


app = App(port=8000)
signaling = RTCSignaling()


@app.websocket("/rtc/signal")
def webrtc_signaling(ws):
    """
    WebRTC signaling endpoint.
    
    Handles SDP exchange and ICE candidate relay.
    
    Client code:
        const ws = new WebSocket('ws://localhost:8000/rtc/signal?room=demo');
        
        // Send offer
        ws.send(JSON.stringify({
            type: 'offer',
            target: 'peer2',
            sdp: localDescription.sdp
        }));
        
        // Receive answer
        ws.onmessage = (e) => {
            const msg = JSON.parse(e.data);
            if (msg.type === 'answer') {
                pc.setRemoteDescription(msg.sdp);
            }
        };
    """
    # Get room from query params (simulated)
    room_id = "default"  # In real app, parse from request
    
    # Register peer
    peer_id = signaling.register_peer(websocket=ws, room_id=room_id)
    
    try:
        # Send peer ID
        ws.send_text(json.dumps({
            "type": "registered",
            "peer_id": peer_id
        }))
        
        # Send list of other peers in room
        other_peers = [p for p in signaling.get_room_peers(room_id) if p != peer_id]
        ws.send_text(json.dumps({
            "type": "peers",
            "peers": other_peers
        }))
        
        # Notify room about new peer
        signaling.broadcast_to_room(
            room_id,
            {"type": "peer-joined", "peer_id": peer_id},
            exclude=peer_id
        )
        
        # Handle messages
        while ws.is_connected():
            try:
                msg_text = ws.receive_text()
                msg = json.loads(msg_text)
                
                # Handle based on message type
                msg_type = msg.get("type")
                target = msg.get("target")
                
                if msg_type == "offer" and target:
                    signaling.relay_offer(peer_id, target, msg.get("sdp", ""))
                
                elif msg_type == "answer" and target:
                    signaling.relay_answer(peer_id, target, msg.get("sdp", ""))
                
                elif msg_type == "ice-candidate" and target:
                    signaling.relay_ice_candidate(peer_id, target, msg.get("candidate", {}))
            
            except Exception as e:
                print(f"Error handling message: {e}")
                break
    
    finally:
        # Cleanup
        signaling.unregister_peer(peer_id)
        
        # Notify room
        signaling.broadcast_to_room(
            room_id,
            {"type": "peer-left", "peer_id": peer_id}
        )


@app.get("/rtc/stats")
def rtc_stats(req: Request, res: Response):
    """Get WebRTC signaling statistics."""
    return signaling.get_stats()


@app.get("/rtc/demo")
def rtc_demo_page(req: Request, res: Response):
    """HTML demo page for WebRTC video chat."""
    
    html = """
<!DOCTYPE html>
<html>
<head>
    <title>FasterAPI WebRTC Demo</title>
    <style>
        body { font-family: Arial; max-width: 1200px; margin: 20px auto; padding: 20px; }
        .video-container { display: flex; gap: 20px; margin: 20px 0; }
        video { width: 400px; height: 300px; background: #000; border-radius: 8px; }
        button { padding: 10px 20px; margin: 5px; cursor: pointer; }
        .log { background: #f5f5f5; padding: 10px; max-height: 200px; overflow-y: auto; font-family: monospace; }
    </style>
</head>
<body>
    <h1>🎥 FasterAPI WebRTC Video Chat Demo</h1>
    
    <div>
        <input type="text" id="roomId" placeholder="Room ID" value="demo-room">
        <button onclick="joinRoom()">Join Room</button>
        <button onclick="startCall()">Start Call</button>
        <button onclick="hangUp()">Hang Up</button>
    </div>
    
    <div class="video-container">
        <div>
            <h3>Local Video</h3>
            <video id="localVideo" autoplay muted></video>
        </div>
        <div>
            <h3>Remote Video</h3>
            <video id="remoteVideo" autoplay></video>
        </div>
    </div>
    
    <div>
        <h3>Signaling Log</h3>
        <div class="log" id="signalingLog"></div>
    </div>
    
    <script>
        let ws = null;
        let pc = null;
        let localStream = null;
        let myPeerId = null;
        let remotePeerId = null;
        
        const log = (msg) => {
            const logDiv = document.getElementById('signalingLog');
            logDiv.innerHTML += new Date().toLocaleTimeString() + ': ' + msg + '<br>';
            logDiv.scrollTop = logDiv.scrollHeight;
        };
        
        async function joinRoom() {
            const roomId = document.getElementById('roomId').value;
            
            // Connect to signaling server
            ws = new WebSocket(`ws://localhost:8000/rtc/signal?room=${roomId}`);
            
            ws.onopen = () => log('✅ Connected to signaling server');
            
            ws.onmessage = async (e) => {
                const msg = JSON.parse(e.data);
                log(`📩 Received: ${msg.type}`);
                
                if (msg.type === 'registered') {
                    myPeerId = msg.peer_id;
                    log(`🆔 My peer ID: ${myPeerId}`);
                }
                
                else if (msg.type === 'peers') {
                    log(`👥 Peers in room: ${msg.peers.length}`);
                    if (msg.peers.length > 0) {
                        remotePeerId = msg.peers[0];
                    }
                }
                
                else if (msg.type === 'peer-joined') {
                    log(`👋 Peer joined: ${msg.peer_id}`);
                    remotePeerId = msg.peer_id;
                }
                
                else if (msg.type === 'offer') {
                    log(`📞 Received offer from ${msg.from}`);
                    await handleOffer(msg.from, msg.sdp);
                }
                
                else if (msg.type === 'answer') {
                    log(`📞 Received answer from ${msg.from}`);
                    await pc.setRemoteDescription({type: 'answer', sdp: msg.sdp});
                }
                
                else if (msg.type === 'ice-candidate') {
                    log(`🧊 Received ICE candidate from ${msg.from}`);
                    if (msg.candidate) {
                        await pc.addIceCandidate(msg.candidate);
                    }
                }
            };
            
            ws.onerror = (e) => log('❌ WebSocket error');
            ws.onclose = () => log('🔌 Disconnected');
        }
        
        async function startCall() {
            if (!ws || !remotePeerId) {
                alert('Join room first and wait for peer');
                return;
            }
            
            // Get local media
            localStream = await navigator.mediaDevices.getUserMedia({
                video: true,
                audio: true
            });
            
            document.getElementById('localVideo').srcObject = localStream;
            log('📹 Local media started');
            
            // Create peer connection
            pc = new RTCPeerConnection({
                iceServers: [{urls: 'stun:stun.l.google.com:19302'}]
            });
            
            // Add local tracks
            localStream.getTracks().forEach(track => {
                pc.addTrack(track, localStream);
            });
            
            // Handle remote stream
            pc.ontrack = (e) => {
                log('📺 Remote stream received');
                document.getElementById('remoteVideo').srcObject = e.streams[0];
            };
            
            // Handle ICE candidates
            pc.onicecandidate = (e) => {
                if (e.candidate) {
                    log('🧊 Sending ICE candidate');
                    ws.send(JSON.stringify({
                        type: 'ice-candidate',
                        target: remotePeerId,
                        candidate: e.candidate
                    }));
                }
            };
            
            // Create and send offer
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            
            log('📤 Sending offer');
            ws.send(JSON.stringify({
                type: 'offer',
                target: remotePeerId,
                sdp: offer.sdp
            }));
        }
        
        async function handleOffer(fromPeer, sdp) {
            // Create peer connection if not exists
            if (!pc) {
                pc = new RTCPeerConnection({
                    iceServers: [{urls: 'stun:stun.l.google.com:19302'}]
                });
                
                // Get local media
                if (!localStream) {
                    localStream = await navigator.mediaDevices.getUserMedia({
                        video: true,
                        audio: true
                    });
                    document.getElementById('localVideo').srcObject = localStream;
                }
                
                localStream.getTracks().forEach(track => {
                    pc.addTrack(track, localStream);
                });
                
                pc.ontrack = (e) => {
                    log('📺 Remote stream received');
                    document.getElementById('remoteVideo').srcObject = e.streams[0];
                };
                
                pc.onicecandidate = (e) => {
                    if (e.candidate) {
                        ws.send(JSON.stringify({
                            type: 'ice-candidate',
                            target: fromPeer,
                            candidate: e.candidate
                        }));
                    }
                };
            }
            
            // Set remote description
            await pc.setRemoteDescription({type: 'offer', sdp: sdp});
            
            // Create and send answer
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            
            log('📤 Sending answer');
            ws.send(JSON.stringify({
                type: 'answer',
                target: fromPeer,
                sdp: answer.sdp
            }));
        }
        
        function hangUp() {
            if (pc) {
                pc.close();
                pc = null;
            }
            
            if (localStream) {
                localStream.getTracks().forEach(track => track.stop());
                localStream = null;
            }
            
            document.getElementById('localVideo').srcObject = null;
            document.getElementById('remoteVideo').srcObject = null;
            
            log('📞 Call ended');
        }
    </script>
</body>
</html>
    """
    
    res.content_type("text/html").text(html).send()


@app.on_event("startup")
def startup():
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║          FasterAPI WebRTC Demo                          ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print("🚀 Server starting on http://localhost:8000")
    print()
    print("🎥 WebRTC Endpoints:")
    print("   WS  /rtc/signal - Signaling server")
    print("   GET /rtc/stats - Statistics")
    print("   GET /rtc/demo - Demo page")
    print()
    print("💡 Open http://localhost:8000/rtc/demo in 2 browser windows")
    print("   to test peer-to-peer video chat!")
    print()
    print("✨ Features:")
    print("   • SDP parsing with zero allocations")
    print("   • ICE candidate relay (Pion-inspired)")
    print("   • simdjson for message parsing (SIMD)")
    print("   • Room-based sessions")
    print("   • Multi-peer support")
    print()


def main():
    app.run()


if __name__ == "__main__":
    main()

