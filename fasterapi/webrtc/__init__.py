"""
WebRTC Support for FasterAPI

Provides WebRTC signaling infrastructure for peer-to-peer connections.
"""

from .signaling import RTCSignaling, RTCPeerSession
from .sdp import SDPSession, SDPParser

__all__ = [
    'RTCSignaling',
    'RTCPeerSession',
    'SDPSession',
    'SDPParser',
]

