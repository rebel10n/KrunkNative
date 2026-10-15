// kn-rtc: WebRTC DataChannel <-> UDP gateway for the KrunkNative web client
// (doc/transport.md). The browser speaks the same knet protocol as the native client
// (shared/src/knet.c compiled to wasm) over one unordered, unreliable DataChannel
// (ordered:false, maxRetransmits:0); every DataChannel message is relayed 1:1 as a UDP
// datagram to the game server's UDP port and back, so reliability, acks and fragmentation
// stay end-to-end in knet and this process holds no game state. Same contract as
// net-bridge/wt (WebTransport), different browser API.
//
// Signaling is WHIP-style and non-trickle: the page POSTs its SDP offer
//
//	POST <path>?target=host:port      Content-Type: application/sdp   (body = offer)
//	201 Created                       Content-Type: application/sdp   (body = answer)
//
// and the answer already carries every candidate: the gateway is ICE-lite and has exactly
// one host candidate per -ice-addr (one fixed UDP port, ICE UDP mux), so there is nothing
// to trickle. The browser's own candidates are not needed either: its connectivity checks
// reach that port and are learned as peer-reflexive candidates. The HTTP side is plain
// HTTP on loopback (behind cloudflared or any TLS proxy); the media path is DTLS/SCTP over
// UDP straight to the gateway, so no certificate and no TURN server are involved.
//
// The one change it makes to the stream: in a knet CONNECT_RESPONSE (byte 0 = 0x03) it
// fills the forwarded-address field with the browser's address (the ICE peer's real UDP
// source), which server_bin trusts only from loopback relays (server/src/net_udp.c), like
// net-bridge's NET_PACKET_FORWARDED and kn-wt.
//
// Targets are allowlisted exactly like net-bridge/webBridge.ts (KRUNK_NATIVE_TARGETS,
// KRUNK_NATIVE_ALLOW_LOOPBACK), so this is not an open UDP proxy; besides, nothing is sent
// to a target before the browser completed ICE + DTLS with the gateway, so it cannot be
// used to reflect traffic at third parties.
package main

import (
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand/v2"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/logging"
	"github.com/pion/webrtc/v4"
)

const (
	knetConnectResponse = 0x03
	knetAccept          = 0x04 // S->C [type][salt4][token4]
	knetData            = 0x06 // both [type][token4]...
	knetDisconnectType  = 0x07 // both [type][token4] (+ one game frame: shared/include/knet.h)
	netPacketError      = 30   // NET_PACKET_ERROR (shared/include/shared.h): NetErrorPacket {message[96], code[8]}
	errorMessageSize    = 96
	errorCodeSize       = 8
	stopReasonMaxAge    = 15 * time.Minute // server_bin ignores an older shutdown_reason_file too
	stopGrace           = 300 * time.Millisecond
	knetForwardedMax    = 47
	maxDatagram         = 1200
	maxOfferBytes       = 32 << 10
	dataChannelLabel    = "knet"
	// Browser-bound bytes the SCTP association may hold before datagrams are dropped (the
	// channel is unreliable anyway; knet resends what matters).
	maxBufferedAmount = 512 << 10
)

// ------------------------------------------------------------------ allowlist (webBridge.ts)

type targetRule struct {
	host   string
	lo, hi int
}

func parseRules(spec string) []targetRule {
	var rules []targetRule
	for _, raw := range strings.Split(spec, ",") {
		item := strings.TrimSpace(raw)
		colon := strings.LastIndex(item, ":")
		if colon <= 0 {
			continue
		}
		host := strings.ToLower(item[:colon])
		ports := item[colon+1:]
		if ports == "*" {
			rules = append(rules, targetRule{host, 1, 65535})
			continue
		}
		a, b, found := strings.Cut(ports, "-")
		lo, err1 := strconv.Atoi(a)
		hi := lo
		var err2 error
		if found {
			hi, err2 = strconv.Atoi(b)
		}
		if err1 == nil && err2 == nil && lo > 0 && hi >= lo {
			rules = append(rules, targetRule{host, lo, hi})
		}
	}
	return rules
}

func isLoopbackAddress(addr string) bool {
	return addr == "::1" || strings.HasPrefix(addr, "127.") || strings.HasPrefix(addr, "::ffff:127.")
}

func isLoopbackTarget(host string) bool { return host == "localhost" || isLoopbackAddress(host) }

type allowlist struct {
	defaultHost   string
	defaultPort   int
	rules         []targetRule
	allowLoopback bool
}

func (a *allowlist) allowed(host string, port int) bool {
	for _, rule := range a.rules {
		if rule.host == host && port >= rule.lo && port <= rule.hi {
			return true
		}
	}
	return false
}

// resolve: webBridge.ts resolveTarget. The default target (no ?target=) has to pass the
// allowlist too; any loopback port only on dev gateways (no KRUNK_NATIVE_TARGETS, or
// KRUNK_NATIVE_ALLOW_LOOPBACK=1).
func (a *allowlist) resolve(target string) (string, bool) {
	if target == "" {
		if a.allowed(strings.ToLower(a.defaultHost), a.defaultPort) {
			return net.JoinHostPort(a.defaultHost, strconv.Itoa(a.defaultPort)), true
		}
		return "", false
	}
	colon := strings.LastIndex(target, ":")
	if colon <= 0 {
		return "", false
	}
	host := strings.ToLower(strings.Trim(target[:colon], "[]"))
	portText := target[colon+1:]
	if len(portText) == 0 || len(portText) > 5 || strings.Trim(portText, "0123456789") != "" {
		return "", false
	}
	port, _ := strconv.Atoi(portText)
	if port <= 0 || port > 65535 {
		return "", false
	}
	join := func(h string) (string, bool) { return net.JoinHostPort(h, strconv.Itoa(port)), true }
	if a.allowed(host, port) {
		return join(host)
	}
	// "localhost" is the IPv4 loopback the game servers listen on (they bind 0.0.0.0 only)
	if host == "localhost" && a.allowed("127.0.0.1", port) {
		return join("127.0.0.1")
	}
	if a.allowLoopback && isLoopbackTarget(host) {
		if host == "localhost" {
			host = "127.0.0.1"
		}
		return join(host)
	}
	return "", false
}

// ------------------------------------------------------------------ knet

// withForwarded rewrites a knet CONNECT_RESPONSE ([0x03][salt4][cookie8][fwd_len][fwd]) so it
// carries the browser's address; every other datagram passes unchanged.
func withForwarded(datagram []byte, host string) []byte {
	if len(datagram) < 14 || datagram[0] != knetConnectResponse || host == "" {
		return datagram
	}
	if len(host) > knetForwardedMax {
		host = host[:knetForwardedMax]
	}
	out := make([]byte, 0, 14+len(host))
	out = append(out, datagram[:13]...)
	out = append(out, byte(len(host)))
	out = append(out, host...)
	return out
}

// ------------------------------------------------------------------ gateway

type gateway struct {
	api            *webrtc.API
	targets        allowlist
	origins        map[string]bool
	clientIPHeader string
	maxPerIP       int
	maxSessions    int
	idleTimeout    time.Duration
	connectTimeout time.Duration
	dropPercent    int // dev loss simulation, both directions (KRUNK_NATIVE_RTC_DROP)

	mu       sync.Mutex
	perIP    map[string]int
	sessions int
	live     sync.Map // id -> *session (closed on shutdown)
	nextID   atomic.Int64
}

// clientAddress: webBridge.ts clientAddress. Behind cloudflared every request comes from
// loopback and the configured header (cf-connecting-ip) names the browser.
func (g *gateway) clientAddress(req *http.Request) string {
	host, _, err := net.SplitHostPort(req.RemoteAddr)
	if err != nil {
		host = req.RemoteAddr
	}
	if g.clientIPHeader != "" && isLoopbackAddress(host) {
		if v := strings.TrimSpace(req.Header.Get(g.clientIPHeader)); v != "" {
			if len(v) > 64 {
				v = v[:64]
			}
			return v
		}
	}
	return host
}

func (g *gateway) admit(client string) (int, string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.maxSessions > 0 && g.sessions >= g.maxSessions {
		return http.StatusServiceUnavailable, "gateway full"
	}
	if g.maxPerIP > 0 && g.perIP[client] >= g.maxPerIP {
		return http.StatusTooManyRequests, "too many connections from this address"
	}
	g.sessions++
	g.perIP[client]++
	return 0, ""
}

func (g *gateway) release(client string) {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.sessions--
	if g.perIP[client]--; g.perIP[client] <= 0 {
		delete(g.perIP, client)
	}
}

func (g *gateway) count() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.sessions
}

func (g *gateway) cors(w http.ResponseWriter, req *http.Request) bool {
	origin := req.Header.Get("Origin")
	if origin == "" {
		return true // not a browser fetch (curl, tests)
	}
	if len(g.origins) > 0 && !g.origins[origin] {
		return false
	}
	h := w.Header()
	h.Set("Access-Control-Allow-Origin", origin)
	h.Set("Vary", "Origin")
	h.Set("Access-Control-Allow-Methods", "POST, OPTIONS")
	h.Set("Access-Control-Allow-Headers", "Content-Type")
	h.Set("Access-Control-Expose-Headers", peerHeader) // the page prints it with its failures
	h.Set("Access-Control-Max-Age", "600")
	return true
}

// peerHeader: on every answer and refusal, how the gateway sees the attempt (peerInfo), so the
// browser's dev console prints the same address, browser and session number as this log.
const peerHeader = "X-KN-RTC-Peer"

// closeNotice: the text message the gateway sends on the data channel just before it closes the
// session (s.close), so the browser can print why ("kn-rtc-close: <reason>"); knet itself only
// uses binary messages.
const closeNotice = "kn-rtc-close: "

func peerInfo(id int64, client, ua string) string {
	session := "-"
	if id > 0 {
		session = fmt.Sprintf("#%d", id)
	}
	if ua == "" {
		ua = "unknown browser"
	}
	return fmt.Sprintf("session %s; address %s; %s", session, client, ua)
}

type session struct {
	g       *gateway
	id      int64
	client  string // signaling address (per-IP cap, logs)
	target  string
	pc      *webrtc.PeerConnection
	udp     *net.UDPConn
	closed  atomic.Bool
	opened  atomic.Bool
	peer    atomic.Value // string: the ICE peer's host (forwarded address)
	fromBrw atomic.Int64 // DataChannel messages received
	toSrv   atomic.Int64 // ... relayed to the server
	fromSrv atomic.Int64 // UDP datagrams received
	toBrw   atomic.Int64 // ... relayed to the browser
	dropped atomic.Int64
	started time.Time
	ua      string // browser family/version from the signaling request (logs)
	// timings since the POST (ms, 0 = not reached) and the ICE state history, for the logs
	tAnswer, tICE, tDTLS, tOpen atomic.Int64
	iceState                    atomic.Value  // string: the last ICE connection state
	iceLost                     atomic.Bool   // ICE went disconnected/failed after connecting
	knetBye                     atomic.Value  // string: who sent the knet DISCONNECT ("server" / "browser")
	token                       atomic.Uint32 // the knet session token (from the server's ACCEPT / DATA), 0 = none yet
	dc                          atomic.Pointer[webrtc.DataChannel]
}

// notifyStop: the gateway is stopping: tell the browser why (its knet goodbye with an ERROR
// frame). 1 = sent (the caller gives it a moment to leave before closing the connection).
func (s *session) notifyStop(message, code string) bool {
	token, dc := s.token.Load(), s.dc.Load()
	if token == 0 || dc == nil || dc.ReadyState() != webrtc.DataChannelStateOpen || s.closed.Load() {
		return false
	}
	return dc.Send(knetByeWithError(token, message, code)) == nil
}

func (s *session) since() int64 { return max(time.Since(s.started).Milliseconds(), 1) }

// knetDisconnect: a knet DISCONNECT datagram ([0x07][token4], possibly with a game frame).
func knetDisconnect(b []byte) bool { return len(b) >= 5 && b[0] == knetDisconnectType }

// knetToken: the session token a server datagram carries (ACCEPT, DATA, DISCONNECT), 0 if none.
func knetToken(b []byte) uint32 {
	switch {
	case len(b) >= 9 && b[0] == knetAccept:
		return binary.BigEndian.Uint32(b[5:9])
	case len(b) >= 15 && b[0] == knetData, len(b) >= 5 && b[0] == knetDisconnectType:
		return binary.BigEndian.Uint32(b[1:5])
	}
	return 0
}

// knetBye: [0x07][token4], the goodbye the gateway sends the game server for a browser that left
// without one (tab closed, network lost, gateway stopping), so its player is freed at once instead
// of after the server's 10 s knet timeout.
func knetBye(token uint32) []byte {
	b := []byte{knetDisconnectType, 0, 0, 0, 0}
	binary.BigEndian.PutUint32(b[1:], token)
	return b
}

// knetByeWithError: the browser's goodbye when the gateway stops: DISCONNECT carrying an ERROR
// frame ([30][len2][NetErrorPacket]) the client shows and acts on like a game server's
// (servers_ui.c: 004004 / 004006 rejoin with a fresh ticket).
func knetByeWithError(token uint32, message, code string) []byte {
	payload := make([]byte, errorMessageSize+errorCodeSize)
	copy(payload[:errorMessageSize-1], message)
	copy(payload[errorMessageSize:errorMessageSize+errorCodeSize-1], code)
	b := append(knetBye(token), netPacketError, byte(len(payload)>>8), byte(len(payload)))
	return append(b, payload...)
}

// stopMessage: what the players are told when the gateway stops, from the first line of
// KRUNK_NATIVE_STOP_REASON_FILE (a deploy writes "update" there around the restart; an older
// file is ignored), like server_bin's server_shutdown_message: update -> "GAME UPDATING"
// (004004), anything else -> "Server restarting" (004006): the client rejoins either way.
func stopMessage(reasonFile string) (string, string) {
	reason := ""
	if reasonFile != "" {
		if st, err := os.Stat(reasonFile); err == nil && time.Since(st.ModTime()) <= stopReasonMaxAge {
			if data, err := os.ReadFile(reasonFile); err == nil {
				reason, _, _ = strings.Cut(string(data), "\n")
				reason = strings.ToLower(strings.TrimSpace(reason))
			}
		}
	}
	if reason == "update" {
		return "GAME UPDATING", "004004"
	}
	return "Server restarting", "004006"
}

// browserFamily: "Chrome/153", "Firefox/131", "Safari/18.1", "Edge/140" or "?" (logs only).
func browserFamily(ua string) string {
	field := func(key string) string {
		i := strings.Index(ua, key)
		if i < 0 {
			return ""
		}
		v := ua[i+len(key):]
		if j := strings.IndexAny(v, " ;)"); j >= 0 {
			v = v[:j]
		}
		if j := strings.Index(v, "."); j >= 0 && key != "Version/" {
			v = v[:j]
		}
		return v
	}
	switch {
	case field("Edg/") != "":
		return "Edge/" + field("Edg/")
	case field("Firefox/") != "":
		return "Firefox/" + field("Firefox/")
	case field("Chrome/") != "":
		return "Chrome/" + field("Chrome/")
	case field("Version/") != "" && strings.Contains(ua, "Safari/"):
		return "Safari/" + field("Version/")
	}
	return "?"
}

// closeReason: what the close callbacks saw, made specific with the session's history.
func (s *session) closeReason(event string) string {
	if who, _ := s.knetBye.Load().(string); who != "" {
		return event + " after the " + who + " ended the knet session"
	}
	if s.iceLost.Load() {
		return event + ": ICE consent lost (the browser went silent: tab closed, sleep or network change)"
	}
	return event
}

func (s *session) drop() bool {
	return s.g.dropPercent > 0 && rand.IntN(100) < s.g.dropPercent
}

func (s *session) close(why string) {
	if s.closed.Swap(true) { // (pc.Close can call back into here)
		return
	}
	// the browser left without a knet goodbye (tab closed, network lost, the gateway stopping):
	// say it for the browser, or the game server keeps the player until its knet timeout
	if who, _ := s.knetBye.Load().(string); who == "" {
		if token := s.token.Load(); token != 0 {
			bye := knetBye(token)
			for i := 0; i < 3; i++ {
				s.udp.Write(bye)
			}
		}
	}
	if dc := s.dc.Load(); dc != nil && dc.ReadyState() == webrtc.DataChannelStateOpen {
		if dc.SendText(closeNotice+why) == nil { // best effort: a lost path (ICE consent) cannot carry it
			time.Sleep(100 * time.Millisecond) // the notice goes out before the association closes
		}
	}
	s.udp.Close()
	s.g.release(s.client)
	s.g.live.Delete(s.id)
	log.Printf("[rtc] #%d %s %s closed: %s (datagrams: %d/%d browser->server, %d/%d server->browser relayed/received, %d buffer drops, %s)",
		s.id, s.client, s.ua, why, s.toSrv.Load(), s.fromBrw.Load(), s.toBrw.Load(), s.fromSrv.Load(), s.dropped.Load(),
		time.Since(s.started).Round(time.Second))
	s.pc.Close()
}

// peerHost: the browser's real UDP source address as ICE selected it (a peer-reflexive
// candidate), else the signaling address.
func (s *session) peerHost() string {
	if v, ok := s.peer.Load().(string); ok && v != "" {
		return v
	}
	if sctp := s.pc.SCTP(); sctp != nil {
		if pair, err := sctp.Transport().ICETransport().GetSelectedCandidatePair(); err == nil && pair != nil && pair.Remote != nil {
			s.peer.Store(pair.Remote.Address)
			return pair.Remote.Address
		}
	}
	return s.client
}

func (s *session) onDataChannel(dc *webrtc.DataChannel) {
	if dc.Label() != dataChannelLabel || s.opened.Swap(true) {
		log.Printf("[rtc] #%d unexpected data channel %q, closing it", s.id, dc.Label())
		dc.Close()
		return
	}
	if dc.Ordered() || dc.MaxRetransmits() == nil {
		log.Printf("[rtc] #%d note: channel is ordered=%v maxRetransmits=%v (want unordered, 0)", s.id, dc.Ordered(), dc.MaxRetransmits())
	}
	dc.OnOpen(func() {
		s.dc.Store(dc)
		s.tOpen.Store(s.since())
		log.Printf("[rtc] #%d %s (ice %s) %s -> udp %s; open in %d ms (answer %d, ICE %d, DTLS %d)", s.id, s.client, s.peerHost(), s.ua, s.target,
			s.tOpen.Load(), s.tAnswer.Load(), s.tICE.Load(), s.tDTLS.Load())
		go s.serverToBrowser(dc)
	})
	dc.OnMessage(func(msg webrtc.DataChannelMessage) {
		s.fromBrw.Add(1)
		if len(msg.Data) == 0 || len(msg.Data) > maxDatagram || s.drop() {
			return
		}
		if knetDisconnect(msg.Data) {
			s.knetBye.CompareAndSwap(nil, "browser")
		}
		if _, err := s.udp.Write(withForwarded(msg.Data, s.peerHost())); err == nil {
			s.toSrv.Add(1)
		}
	})
	dc.OnClose(func() { s.close(s.closeReason("data channel closed")) })
}

func (s *session) serverToBrowser(dc *webrtc.DataChannel) {
	buf := make([]byte, 2048)
	for {
		s.udp.SetReadDeadline(time.Now().Add(s.g.idleTimeout))
		n, err := s.udp.Read(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				s.close(fmt.Sprintf("no datagram from %s for %s", s.target, s.g.idleTimeout))
			} else if !s.closed.Load() {
				s.close(s.closeReason("udp from " + s.target + ": " + err.Error())) // e.g. connection refused: the game server is down
			}
			return
		}
		s.fromSrv.Add(1)
		if knetDisconnect(buf[:n]) {
			s.knetBye.CompareAndSwap(nil, "server")
		} else if s.token.Load() == 0 {
			s.token.Store(knetToken(buf[:n]))
		}
		if n > maxDatagram || s.drop() {
			continue
		}
		if dc.BufferedAmount() > maxBufferedAmount {
			s.dropped.Add(1)
			continue
		}
		if err := dc.Send(buf[:n]); err != nil {
			s.close("send: " + err.Error())
			return
		}
		s.toBrw.Add(1)
	}
}

func (g *gateway) handle(w http.ResponseWriter, req *http.Request) {
	if !g.cors(w, req) {
		log.Printf("[rtc] %s refused: origin %q not allowed", g.clientAddress(req), req.Header.Get("Origin"))
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return
	}
	if req.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if req.Method != http.MethodPost {
		w.Header().Set("Allow", "POST, OPTIONS")
		http.Error(w, "POST an SDP offer", http.StatusMethodNotAllowed)
		return
	}
	client := g.clientAddress(req)
	w.Header().Set(peerHeader, peerInfo(0, client, browserFamily(req.UserAgent())))
	target, ok := g.targets.resolve(req.URL.Query().Get("target"))
	if !ok {
		log.Printf("[rtc] %s refused: target not allowed (%s)", client, req.URL.RawQuery)
		http.Error(w, "target not allowed", http.StatusForbidden)
		return
	}
	offer, err := io.ReadAll(io.LimitReader(req.Body, maxOfferBytes+1))
	if err != nil || len(offer) == 0 || len(offer) > maxOfferBytes {
		http.Error(w, "bad offer", http.StatusBadRequest)
		return
	}
	if status, reason := g.admit(client); status != 0 {
		log.Printf("[rtc] %s refused: %s", client, reason)
		http.Error(w, reason, status)
		return
	}

	s := &session{g: g, id: g.nextID.Add(1), client: client, target: target, started: time.Now(), ua: browserFamily(req.UserAgent())}
	w.Header().Set(peerHeader, peerInfo(s.id, client, s.ua))
	g.live.Store(s.id, s)
	fail := func(status int, msg string, err error) {
		log.Printf("[rtc] #%d %s: %s: %v", s.id, client, msg, err)
		if s.pc != nil {
			s.pc.Close()
		}
		if s.udp != nil {
			s.udp.Close()
		}
		g.live.Delete(s.id)
		g.release(client)
		http.Error(w, msg, status)
	}
	addr, err := net.ResolveUDPAddr("udp4", target)
	if err != nil {
		fail(http.StatusBadGateway, "bad target", err)
		return
	}
	if s.udp, err = net.DialUDP("udp4", nil, addr); err != nil {
		fail(http.StatusBadGateway, "target unreachable", err)
		return
	}
	if s.pc, err = g.api.NewPeerConnection(webrtc.Configuration{}); err != nil {
		fail(http.StatusInternalServerError, "peer connection", err)
		return
	}
	s.pc.OnDataChannel(s.onDataChannel)
	s.pc.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		s.iceState.Store(state.String())
		switch state {
		case webrtc.ICEConnectionStateConnected:
			s.tICE.CompareAndSwap(0, s.since())
		case webrtc.ICEConnectionStateDisconnected, webrtc.ICEConnectionStateFailed:
			if s.tICE.Load() > 0 {
				s.iceLost.Store(true)
			}
		}
	})
	s.pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		if state == webrtc.PeerConnectionStateConnected {
			s.tDTLS.CompareAndSwap(0, s.since())
		}
		if state == webrtc.PeerConnectionStateFailed || state == webrtc.PeerConnectionStateClosed {
			s.close(s.closeReason("peer connection " + state.String()))
		}
	})
	if err = s.pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeOffer, SDP: string(offer)}); err != nil {
		fail(http.StatusBadRequest, "bad offer", err)
		return
	}
	answer, err := s.pc.CreateAnswer(nil)
	if err != nil {
		fail(http.StatusBadRequest, "cannot answer", err)
		return
	}
	gathered := webrtc.GatheringCompletePromise(s.pc)
	if err = s.pc.SetLocalDescription(answer); err != nil {
		fail(http.StatusInternalServerError, "local description", err)
		return
	}
	select {
	case <-gathered: // host candidates on the mux only: immediate
	case <-time.After(3 * time.Second):
		fail(http.StatusInternalServerError, "candidate gathering", fmt.Errorf("timed out"))
		return
	}
	s.tAnswer.Store(s.since())
	// ICE + DTLS + the data channel have connectTimeout to come up. The log says how far it got:
	// no ICE check at all means UDP from the browser never reached this port.
	time.AfterFunc(g.connectTimeout, func() {
		if s.opened.Load() {
			return
		}
		checks := 0
		for _, st := range s.pc.GetStats() {
			if p, ok := st.(webrtc.ICECandidatePairStats); ok {
				checks += int(p.RequestsReceived)
			}
		}
		ice, _ := s.iceState.Load().(string)
		why := fmt.Sprintf("no data channel within %s (ICE %s, %d checks received", g.connectTimeout, ice, checks)
		switch {
		case checks == 0:
			why += ": no UDP from the browser reached the gateway; blocked on its network, or it gave up)"
		case s.tICE.Load() == 0:
			why += ": checks arrived but ICE did not complete)"
		case s.tDTLS.Load() == 0:
			why += fmt.Sprintf(": ICE up at %d ms, DTLS did not complete)", s.tICE.Load())
		default:
			why += fmt.Sprintf(": DTLS up at %d ms, no data channel)", s.tDTLS.Load())
		}
		s.close(why)
	})
	w.Header().Set("Content-Type", "application/sdp")
	w.WriteHeader(http.StatusCreated)
	io.WriteString(w, s.pc.LocalDescription().SDP)
}

// ------------------------------------------------------------------ setup

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func envInt(name string, fallback int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil && v >= 0 {
		return v
	}
	return fallback
}

// iceMux: one UDP port for every session. A specific IP gets a single socket (the
// candidate is that IP); a wildcard address binds the port on every interface address.
func iceMux(addr string, loopback bool) (ice.UDPMux, error) {
	host, portText, err := net.SplitHostPort(addr)
	if err != nil {
		return nil, err
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port <= 0 {
		return nil, fmt.Errorf("bad port in %q", addr)
	}
	if ip := net.ParseIP(host); ip != nil && !ip.IsUnspecified() {
		conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: ip, Port: port})
		if err != nil {
			return nil, err
		}
		return ice.NewUDPMuxDefault(ice.UDPMuxParams{UDPConn: conn}), nil
	}
	opts := []ice.UDPMuxFromPortOption{ice.UDPMuxFromPortWithNetworks(ice.NetworkTypeUDP4)}
	if loopback {
		opts = append(opts, ice.UDPMuxFromPortWithLoopback())
	}
	return ice.NewMultiUDPMuxFromPort(port, opts...)
}

func main() {
	httpAddr := flag.String("http", envOr("KRUNK_NATIVE_RTC_HTTP", "127.0.0.1:8085"), "signaling HTTP listen address (put TLS / cloudflared in front)")
	path := flag.String("path", envOr("KRUNK_NATIVE_RTC_PATH", "/rtc"), "signaling path (POST the SDP offer here)")
	iceAddr := flag.String("ice-addr", envOr("KRUNK_NATIVE_RTC_ICE_ADDR", ":21100"), "the one UDP address for ICE/DTLS (a public IP, or :port for every interface)")
	publicIP := flag.String("public-ip", os.Getenv("KRUNK_NATIVE_RTC_PUBLIC_IP"), "comma-separated IPs to advertise instead of the bound address (1:1 NAT)")
	origins := flag.String("origins", os.Getenv("KRUNK_NATIVE_RTC_ORIGINS"), "comma-separated allowed Origin values; empty = any (dev)")
	infoFile := flag.String("info-file", os.Getenv("KRUNK_NATIVE_RTC_INFO"), "write {\"url\"} JSON here (the web client reads /rtc.json)")
	stopReasonFile := flag.String("stop-reason-file", os.Getenv("KRUNK_NATIVE_STOP_REASON_FILE"), "on SIGTERM, \"update\" in its first line tells players GAME UPDATING (else Server restarting)")
	publicURL := flag.String("public-url", os.Getenv("KRUNK_NATIVE_RTC_URL"), "URL advertised in -info-file (default http://<http><path>)")
	verbose := flag.Bool("v", os.Getenv("KRUNK_NATIVE_RTC_DEBUG") != "", "pion debug logging")
	flag.Parse()

	nativeHost := envOr("KRUNK_NATIVE_HOST", "127.0.0.1")
	nativePort, err := strconv.Atoi(envOr("KRUNK_NATIVE_PORT", "21015"))
	if err != nil || nativePort <= 0 {
		nativePort = 21015
	}
	targetSpec := os.Getenv("KRUNK_NATIVE_TARGETS")
	allowLoopback := targetSpec == ""
	if v, set := os.LookupEnv("KRUNK_NATIVE_ALLOW_LOOPBACK"); set {
		switch strings.ToLower(v) {
		case "1", "true", "yes", "on":
			allowLoopback = true
		default:
			allowLoopback = false
		}
	}
	if targetSpec == "" {
		targetSpec = fmt.Sprintf("%s:%d-%d", nativeHost, nativePort, nativePort+63)
	}

	g := &gateway{
		targets:        allowlist{defaultHost: nativeHost, defaultPort: nativePort, rules: parseRules(targetSpec), allowLoopback: allowLoopback},
		origins:        map[string]bool{},
		clientIPHeader: strings.ToLower(strings.TrimSpace(os.Getenv("KRUNK_NATIVE_CLIENT_IP_HEADER"))),
		maxPerIP:       envInt("KRUNK_NATIVE_MAX_PER_IP", 0),
		maxSessions:    envInt("KRUNK_NATIVE_MAX_SESSIONS", 0),
		idleTimeout:    time.Duration(envInt("KRUNK_NATIVE_RTC_IDLE_TIMEOUT_MS", 30000)) * time.Millisecond,
		connectTimeout: time.Duration(envInt("KRUNK_NATIVE_CONNECT_TIMEOUT_MS", 10000)) * time.Millisecond,
		dropPercent:    min(envInt("KRUNK_NATIVE_RTC_DROP", 0), 100),
		perIP:          map[string]int{},
	}
	for _, o := range strings.Split(*origins, ",") {
		if o = strings.TrimSpace(o); o != "" {
			g.origins[o] = true
		}
	}

	mux, err := iceMux(*iceAddr, allowLoopback)
	if err != nil {
		log.Fatalf("[rtc] ICE UDP %s: %v", *iceAddr, err)
	}
	se := webrtc.SettingEngine{}
	se.SetLite(true) // public address, host candidates only: the browser runs the checks
	se.SetICEUDPMux(mux)
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled) // browsers' .local names mean nothing here
	se.SetIncludeLoopbackCandidate(allowLoopback)
	se.SetICETimeouts(5*time.Second, 10*time.Second, 2*time.Second)
	if ips := strings.FieldsFunc(*publicIP, func(r rune) bool { return r == ',' || r == ' ' }); len(ips) > 0 {
		se.SetNAT1To1IPs(ips, webrtc.ICECandidateTypeHost)
	}
	if *verbose {
		lf := logging.NewDefaultLoggerFactory()
		lf.DefaultLogLevel = logging.LogLevelDebug
		se.LoggerFactory = lf
	}
	g.api = webrtc.NewAPI(webrtc.WithSettingEngine(se))

	httpMux := http.NewServeMux()
	httpMux.HandleFunc(*path, g.handle)
	httpMux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintf(w, "{\"ok\":true,\"sessions\":%d}\n", g.count())
	})
	srv := &http.Server{Addr: *httpAddr, Handler: httpMux, ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 15 * time.Second, WriteTimeout: 15 * time.Second}

	url := *publicURL
	if url == "" {
		url = "http://" + *httpAddr + *path
	}
	if *infoFile != "" {
		info, _ := json.Marshal(map[string]string{"url": url})
		if err := os.WriteFile(*infoFile, info, 0o644); err != nil {
			log.Printf("[rtc] cannot write %s: %v", *infoFile, err)
		}
	}
	log.Printf("[rtc] signaling %s (http %s), ICE udp %s%s -> native udp (targets %s%s); limits per-ip %d, total %d, idle %s%s",
		url, *httpAddr, *iceAddr, map[bool]string{true: " advertised as " + *publicIP, false: ""}[*publicIP != ""],
		targetSpec, map[bool]string{true: " + any loopback (dev)", false: ""}[allowLoopback],
		g.maxPerIP, g.maxSessions, g.idleTimeout, map[bool]string{true: fmt.Sprintf(", DROP %d%% (test)", g.dropPercent), false: ""}[g.dropPercent > 0])

	stop := make(chan os.Signal, 1)
	done := make(chan struct{})
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		message, code := stopMessage(*stopReasonFile)
		log.Printf("[rtc] stopping (%d sessions): telling players %q (%s)", g.count(), message, code)
		srv.Close()
		// every browser gets the reason first (its client shows it and rejoins with a fresh ticket),
		// then a moment for those datagrams to leave before the connections close
		told := 0
		g.live.Range(func(_, v any) bool {
			if v.(*session).notifyStop(message, code) {
				told++
			}
			return true
		})
		if told > 0 {
			time.Sleep(stopGrace)
		}
		g.live.Range(func(_, v any) bool {
			v.(*session).close(fmt.Sprintf("gateway stopping (told %q)", message))
			return true
		})
		mux.Close()
		close(done)
	}()
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("[rtc] %v", err)
	}
	<-done
}
