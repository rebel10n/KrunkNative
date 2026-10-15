package main

import (
	"bytes"
	"encoding/binary"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestKnetToken(t *testing.T) {
	accept := []byte{knetAccept, 1, 2, 3, 4, 0xde, 0xad, 0xbe, 0xef}
	if got := knetToken(accept); got != 0xdeadbeef {
		t.Fatalf("ACCEPT token %08x", got)
	}
	data := make([]byte, 15)
	data[0] = knetData
	binary.BigEndian.PutUint32(data[1:], 7)
	if got := knetToken(data); got != 7 {
		t.Fatalf("DATA token %d", got)
	}
	if got := knetToken([]byte{0x02, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}); got != 0 {
		t.Fatalf("CHALLENGE has no token, got %d", got)
	}
	if got := knetToken(data[:10]); got != 0 {
		t.Fatalf("short DATA token %d", got)
	}
}

// The browser's goodbye when the gateway stops: knet DISCONNECT + one NET_PACKET_ERROR frame
// (shared/include/knet.h KNET_DISCONNECT, NetErrorPacket = message[96] + code[8]).
func TestByeWithError(t *testing.T) {
	b := knetByeWithError(0x01020304, "GAME UPDATING", "004004")
	if len(b) != 5+3+104 || b[0] != knetDisconnectType || binary.BigEndian.Uint32(b[1:5]) != 0x01020304 {
		t.Fatalf("header % x", b[:8])
	}
	if b[5] != netPacketError || int(b[6])<<8|int(b[7]) != 104 {
		t.Fatalf("frame header % x", b[5:8])
	}
	msg, code := b[8:8+96], b[8+96:]
	if string(bytes.TrimRight(msg, "\x00")) != "GAME UPDATING" || string(bytes.TrimRight(code, "\x00")) != "004004" {
		t.Fatalf("payload %q %q", msg, code)
	}
	if msg[95] != 0 || code[7] != 0 {
		t.Fatal("strings not terminated")
	}
	long := knetByeWithError(1, string(bytes.Repeat([]byte("x"), 200)), "0123456789")
	if len(long) != len(b) || long[8+95] != 0 || long[8+96+7] != 0 {
		t.Fatal("long strings not cut to the struct")
	}
	if !knetDisconnect(b) || !knetDisconnect(knetBye(1)) || knetDisconnect([]byte{knetDisconnectType, 1}) {
		t.Fatal("knetDisconnect")
	}
}

func TestStopMessage(t *testing.T) {
	if m, c := stopMessage(""); m != "Server restarting" || c != "004006" {
		t.Fatalf("no file: %q %q", m, c)
	}
	f := filepath.Join(t.TempDir(), "reason")
	os.WriteFile(f, []byte("update\n"), 0o644)
	if m, c := stopMessage(f); m != "GAME UPDATING" || c != "004004" {
		t.Fatalf("update: %q %q", m, c)
	}
	old := time.Now().Add(-stopReasonMaxAge - time.Minute)
	os.Chtimes(f, old, old)
	if m, _ := stopMessage(f); m != "Server restarting" {
		t.Fatalf("stale file used: %q", m)
	}
	os.WriteFile(f, []byte("restart"), 0o644)
	if m, c := stopMessage(f); m != "Server restarting" || c != "004006" {
		t.Fatalf("restart: %q %q", m, c)
	}
}

// Every signaling answer, a refusal included, tells the page how the gateway saw it (address,
// browser, session) in X-KN-RTC-Peer, exposed to the page's fetch for its dev-console failures.
func TestPeerHeaderOnRefusal(t *testing.T) {
	g := &gateway{targets: allowlist{defaultHost: "127.0.0.1", defaultPort: 21015, rules: parseRules("127.0.0.1:21015")}, perIP: map[string]int{}}
	req := httptest.NewRequest(http.MethodPost, "/rtc?target=10.0.0.9:21015", strings.NewReader("v=0"))
	req.RemoteAddr = "5.188.124.57:40000"
	req.Header.Set("Origin", "https://krunk.io")
	req.Header.Set("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36")
	rec := httptest.NewRecorder()
	g.handle(rec, req)
	if rec.Code != http.StatusForbidden || !strings.Contains(rec.Body.String(), "target not allowed") {
		t.Fatalf("refusal %d %q", rec.Code, rec.Body.String())
	}
	peer := rec.Header().Get(peerHeader)
	if !strings.Contains(peer, "address 5.188.124.57") || !strings.Contains(peer, "Chrome/153") || !strings.Contains(peer, "session -") {
		t.Fatalf("peer header %q", peer)
	}
	if !strings.Contains(rec.Header().Get("Access-Control-Expose-Headers"), peerHeader) {
		t.Fatalf("the page cannot read %s: expose %q", peerHeader, rec.Header().Get("Access-Control-Expose-Headers"))
	}
	if got := peerInfo(12, "1.2.3.4", ""); got != "session #12; address 1.2.3.4; unknown browser" {
		t.Fatalf("peerInfo %q", got)
	}
}
