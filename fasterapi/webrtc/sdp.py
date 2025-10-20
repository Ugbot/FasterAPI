"""
SDP (Session Description Protocol) utilities.
"""

from typing import Dict, List, Optional
from dataclasses import dataclass, field


@dataclass
class SDPMedia:
    """SDP media description."""
    media_type: str
    port: int
    protocol: str
    formats: List[str] = field(default_factory=list)
    attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class SDPSession:
    """SDP session description."""
    version: str = "0"
    origin: str = ""
    session_name: str = "-"
    connection: str = ""
    timing: str = "0 0"
    
    media: List[SDPMedia] = field(default_factory=list)
    attributes: Dict[str, str] = field(default_factory=dict)


class SDPParser:
    """SDP parser and generator."""
    
    @staticmethod
    def parse(sdp_text: str) -> SDPSession:
        """Parse SDP text."""
        session = SDPSession()
        current_media: Optional[SDPMedia] = None
        
        for line in sdp_text.strip().split('\n'):
            line = line.strip()
            if not line or '=' not in line:
                continue
            
            line_type, value = line.split('=', 1)
            value = value.strip()
            
            if line_type == 'v':
                session.version = value
            elif line_type == 'o':
                session.origin = value
            elif line_type == 's':
                session.session_name = value
            elif line_type == 'c':
                session.connection = value
            elif line_type == 't':
                session.timing = value
            elif line_type == 'm':
                parts = value.split()
                if len(parts) >= 3:
                    media = SDPMedia(
                        media_type=parts[0],
                        port=int(parts[1]),
                        protocol=parts[2],
                        formats=parts[3:] if len(parts) > 3 else []
                    )
                    session.media.append(media)
                    current_media = media
            elif line_type == 'a':
                if ':' in value:
                    attr_name, attr_value = value.split(':', 1)
                    if current_media:
                        current_media.attributes[attr_name] = attr_value
                    else:
                        session.attributes[attr_name] = attr_value
                else:
                    if current_media:
                        current_media.attributes[value] = ""
                    else:
                        session.attributes[value] = ""
        
        return session
    
    @staticmethod
    def generate(session: SDPSession) -> str:
        """Generate SDP text."""
        lines = []
        
        lines.append(f"v={session.version}")
        lines.append(f"o={session.origin}")
        lines.append(f"s={session.session_name}")
        if session.connection:
            lines.append(f"c={session.connection}")
        lines.append(f"t={session.timing}")
        
        for key, value in session.attributes.items():
            if value:
                lines.append(f"a={key}:{value}")
            else:
                lines.append(f"a={key}")
        
        for media in session.media:
            media_line = f"m={media.media_type} {media.port} {media.protocol}"
            if media.formats:
                media_line += " " + " ".join(media.formats)
            lines.append(media_line)
            
            for key, value in media.attributes.items():
                if value:
                    lines.append(f"a={key}:{value}")
                else:
                    lines.append(f"a={key}")
        
        return "\r\n".join(lines) + "\r\n"
