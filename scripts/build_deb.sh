#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
DEB_DIR="$PROJECT_DIR/deb"
VERSION="2.0.0"
PACKAGE_NAME="lan-chat_${VERSION}_amd64.deb"
OUTPUT_DIR="$PROJECT_DIR/dist"

echo "=== Building LAN Chat P2P DEB ==="

# 1. Build
echo "[1/4] Building project..."
cd "$PROJECT_DIR"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" -j$(nproc)

# 2. Strip
echo "[2/4] Stripping..."
strip "$BUILD_DIR/client/lan-chat" 2>/dev/null || true

# 3. Package structure
echo "[3/4] Creating package..."
rm -rf "$DEB_DIR/usr" "$DEB_DIR/etc" "$DEB_DIR/var"
mkdir -p "$DEB_DIR/usr/bin"
mkdir -p "$DEB_DIR/usr/share/applications"
mkdir -p "$DEB_DIR/usr/share/icons/hicolor/128x128/apps"

cp "$BUILD_DIR/client/lan-chat" "$DEB_DIR/usr/bin/"

cat > "$DEB_DIR/usr/share/applications/lan-chat.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=LAN Chat
Name[zh_CN]=局域网聊天
GenericName=P2P Instant Messenger
GenericName[zh_CN]=P2P即时通讯
Comment=Serverless P2P LAN chat
Comment[zh_CN]=无需服务器的P2P局域网聊天
Exec=lan-chat
Icon=lan-chat
Categories=Network;InstantMessaging;Chat;
Keywords=chat;lan;p2p;内网;聊天;
Terminal=false
StartupNotify=true
EOF

# Generate icon
python3 << 'ICON_EOF'
import struct, zlib, os
w, h = 128, 128
pixels = []
cx, cy, rx, ry = 64, 58, 50, 36
for y in range(h):
    for x in range(w):
        dx, dy = (x-cx)/rx, (y-cy)/ry
        in_bubble = dx*dx + dy*dy <= 1.0
        in_tail = False
        if 80 <= y <= 108:
            ty = (y-80)/28.0
            if (32 - int(ty*18)) <= x <= (64 + int(ty*16)):
                in_tail = True
        if in_tail:
            pixels.append((138, 43, 226, 255))
        elif in_bubble:
            r = dx*dx + dy*dy
            pixels.append((106, 13, 173, 255) if r < 0.7 else (138, 43, 226, 255))
        else:
            pixels.append((26, 26, 46, 0))

def chunk(typ, data):
    c = typ + data
    crc = zlib.crc32(c) & 0xffffffff
    return struct.pack('>I', len(data)) + c + struct.pack('>I', crc)

ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
raw = b''
for i in range(0, len(pixels), w):
    raw += b'\x00'
    for j in range(w):
        raw += struct.pack('BBBB', *pixels[i+j])
compressed = zlib.compress(raw)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', compressed) + chunk(b'IEND', b'')
with open('/tmp/lan-chat.png', 'wb') as f: f.write(png)
print(f"Icon: {len(png)} bytes")
ICON_EOF

mv /tmp/lan-chat.png "$DEB_DIR/usr/share/icons/hicolor/128x128/apps/lan-chat.png"

# 4. Build DEB
echo "[4/4] Building DEB..."
chmod 755 "$DEB_DIR/DEBIAN/postinst" "$DEB_DIR/DEBIAN/prerm" "$DEB_DIR/usr/bin/lan-chat"
chmod 644 "$DEB_DIR/usr/share/applications/lan-chat.desktop"

mkdir -p "$OUTPUT_DIR"
dpkg-deb --build "$DEB_DIR" "$OUTPUT_DIR/$PACKAGE_NAME"

echo ""
echo "=== $OUTPUT_DIR/$PACKAGE_NAME ==="
dpkg-deb --info "$OUTPUT_DIR/$PACKAGE_NAME"
