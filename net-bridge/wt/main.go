// kn-wt: WebTransport (HTTP/3) <-> UDP relay for the KrunkNative web client
// (doc/transport.md). The browser speaks the same knet protocol as the native client
// (shared/src/knet.c compiled to wasm); every WebTransport datagram is relayed 1:1 to the
// game server's UDP port and back, so reliability, acks and fragmentation stay end-to-end
// in knet and this process holds no game state.
//
// The one change it makes: in a knet CONNECT_RESPONSE (byte 0 = 0x03) it fills the
// forwarded-address field with the browser's real address, which server_bin trusts only
// from loopback relays (server/src/net_udp.c), like net-bridge's NET_PACKET_FORWARDED.
//
//	GET https://<addr>/native?target=host:port   (CONNECT, protocol webtransport)
//
// Targets are allowlisted (KRUNK_NATIVE_TARGETS, same syntax and default as
// net-bridge/webBridge.ts), so this is not an open UDP proxy.
//
// TLS: -cert/-key for a real certificate (edge hosts). Without them a self-signed ECDSA
// certificate valid for 13 days is generated at startup and its SHA-256 is published
// for serverCertificateHashes in -info-file (default: none) and on stdout.
package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/webtransport-go"
)

const (
	knetConnectResponse = 0x03
	knetForwardedMax    = 47
	maxDatagram         = 1200
	idleTimeout         = 30 * time.Second
)

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

type relay struct {
	defaultHost string
	defaultPort int
	rules       []targetRule
	sessions    atomic.Int64
	nextID      atomic.Int64
}

func (r *relay) resolveTarget(req *http.Request) (string, bool) {
	target := req.URL.Query().Get("target")
	if target == "" {
		return net.JoinHostPort(r.defaultHost, strconv.Itoa(r.defaultPort)), true
	}
	colon := strings.LastIndex(target, ":")
	if colon <= 0 {
		return "", false
	}
	host := strings.ToLower(strings.Trim(target[:colon], "[]"))
	port, err := strconv.Atoi(target[colon+1:])
	if err != nil || port <= 0 || port > 65535 {
		return "", false
	}
	for _, rule := range r.rules {
		if rule.host == host && port >= rule.lo && port <= rule.hi {
			return net.JoinHostPort(host, strconv.Itoa(port)), true
		}
	}
	return "", false
}

// withForwarded rewrites a knet CONNECT_RESPONSE ([0x03][salt4][cookie8][fwd_len][fwd]) so it
// carries the browser's address; every other datagram passes unchanged.
func withForwarded(datagram []byte, remote string) []byte {
	if len(datagram) < 14 || datagram[0] != knetConnectResponse {
		return datagram
	}
	host, _, err := net.SplitHostPort(remote)
	if err != nil {
		host = remote
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

func (r *relay) serve(sess *webtransport.Session, target, remote string) {
	id := r.nextID.Add(1)
	r.sessions.Add(1)
	defer r.sessions.Add(-1)

	addr, err := net.ResolveUDPAddr("udp", target)
	if err != nil {
		log.Printf("[wt] #%d %s: resolve %s: %v", id, remote, target, err)
		sess.CloseWithError(1, "bad target")
		return
	}
	udp, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		log.Printf("[wt] #%d %s: dial %s: %v", id, remote, target, err)
		sess.CloseWithError(1, "target unreachable")
		return
	}
	defer udp.Close()
	log.Printf("[wt] #%d %s -> udp %s", id, remote, target)

	ctx, cancel := context.WithCancel(sess.Context())
	defer cancel()
	var toServer, toBrowser atomic.Int64

	// server -> browser
	go func() {
		defer cancel()
		buf := make([]byte, 2048)
		for {
			udp.SetReadDeadline(time.Now().Add(idleTimeout))
			n, err := udp.Read(buf)
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					log.Printf("[wt] #%d no datagram from %s for %s", id, target, idleTimeout)
				}
				return
			}
			if n > maxDatagram {
				continue
			}
			if err := sess.SendDatagram(buf[:n]); err != nil {
				return
			}
			toBrowser.Add(1)
		}
	}()

	// browser -> server
	for {
		datagram, err := sess.ReceiveDatagram(ctx)
		if err != nil {
			break
		}
		if _, err := udp.Write(withForwarded(datagram, remote)); err != nil {
			break
		}
		toServer.Add(1)
	}
	sess.CloseWithError(0, "")
	log.Printf("[wt] #%d %s closed (%d datagrams to the server, %d to the browser)", id, remote, toServer.Load(), toBrowser.Load())
}

// selfSigned: an ECDSA P-256 certificate valid for 13 days (the serverCertificateHashes
// limit is 14) for localhost, 127.0.0.1, ::1 and extra hosts.
func selfSigned(hosts []string) (tls.Certificate, string, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	serial, _ := rand.Int(rand.Reader, big.NewInt(1<<62))
	now := time.Now().Add(-time.Hour)
	tmpl := x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "krunknative-wt"},
		NotBefore:             now,
		NotAfter:              now.Add(13 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              []string{"localhost"},
		IPAddresses:           []net.IP{net.IPv4(127, 0, 0, 1), net.IPv6loopback},
	}
	for _, h := range hosts {
		if ip := net.ParseIP(h); ip != nil {
			tmpl.IPAddresses = append(tmpl.IPAddresses, ip)
		} else if h != "" {
			tmpl.DNSNames = append(tmpl.DNSNames, h)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	sum := sha256.Sum256(der)
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, base64.StdEncoding.EncodeToString(sum[:]), nil
}

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	addr := flag.String("addr", envOr("KRUNK_NATIVE_WT_ADDR", ":8443"), "UDP listen address for HTTP/3 + WebTransport")
	path := flag.String("path", envOr("KRUNK_NATIVE_WT_PATH", "/native"), "WebTransport endpoint path")
	certFile := flag.String("cert", os.Getenv("KRUNK_NATIVE_WT_CERT"), "TLS certificate (PEM); empty = self-signed dev certificate")
	keyFile := flag.String("key", os.Getenv("KRUNK_NATIVE_WT_KEY"), "TLS private key (PEM)")
	certHosts := flag.String("cert-hosts", os.Getenv("KRUNK_NATIVE_WT_CERT_HOSTS"), "extra comma-separated names/IPs for the self-signed certificate")
	infoFile := flag.String("info-file", os.Getenv("KRUNK_NATIVE_WT_INFO"), "write {\"url\",\"certHash\"} JSON here (the web client reads /wt.json)")
	publicURL := flag.String("public-url", os.Getenv("KRUNK_NATIVE_WT_URL"), "URL advertised in -info-file (default https://127.0.0.1<addr><path>)")
	origins := flag.String("origins", os.Getenv("KRUNK_NATIVE_WT_ORIGINS"), "comma-separated allowed Origin values; empty = any (dev)")
	flag.Parse()

	nativeHost := envOr("KRUNK_NATIVE_HOST", "127.0.0.1")
	nativePort, err := strconv.Atoi(envOr("KRUNK_NATIVE_PORT", "21015"))
	if err != nil || nativePort <= 0 {
		nativePort = 21015
	}
	r := &relay{
		defaultHost: nativeHost,
		defaultPort: nativePort,
		rules:       parseRules(envOr("KRUNK_NATIVE_TARGETS", fmt.Sprintf("%s:%d-%d", nativeHost, nativePort, nativePort+63))),
	}

	var cert tls.Certificate
	certHash := ""
	if *certFile != "" {
		cert, err = tls.LoadX509KeyPair(*certFile, *keyFile)
	} else {
		cert, certHash, err = selfSigned(strings.Split(*certHosts, ","))
	}
	if err != nil {
		log.Fatalf("[wt] certificate: %v", err)
	}

	allowed := map[string]bool{}
	for _, o := range strings.Split(*origins, ",") {
		if o = strings.TrimSpace(o); o != "" {
			allowed[o] = true
		}
	}

	h3 := &http3.Server{
		Addr:      *addr,
		TLSConfig: http3.ConfigureTLSConfig(&tls.Config{Certificates: []tls.Certificate{cert}}),
		QUICConfig: &quic.Config{
			EnableDatagrams: true,
			MaxIdleTimeout:  idleTimeout,
			KeepAlivePeriod: 10 * time.Second,
		},
	}
	webtransport.ConfigureHTTP3Server(h3)
	wt := &webtransport.Server{
		H3: h3,
		CheckOrigin: func(req *http.Request) bool {
			if len(allowed) == 0 {
				return true
			}
			return allowed[req.Header.Get("Origin")]
		},
	}
	mux := http.NewServeMux()
	mux.HandleFunc(*path, func(w http.ResponseWriter, req *http.Request) {
		target, ok := r.resolveTarget(req)
		if !ok {
			log.Printf("[wt] %s refused: target not allowed (%s)", req.RemoteAddr, req.URL.RawQuery)
			w.WriteHeader(http.StatusForbidden)
			return
		}
		sess, err := wt.Upgrade(w, req)
		if err != nil {
			log.Printf("[wt] %s upgrade failed: %v", req.RemoteAddr, err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		r.serve(sess, target, req.RemoteAddr)
	})
	h3.Handler = mux

	url := *publicURL
	if url == "" {
		_, port, _ := net.SplitHostPort(*addr)
		url = "https://127.0.0.1:" + port + *path
	}
	if *infoFile != "" {
		info, _ := json.Marshal(map[string]string{"url": url, "certHash": certHash})
		if err := os.WriteFile(*infoFile, info, 0o644); err != nil {
			log.Printf("[wt] cannot write %s: %v", *infoFile, err)
		}
	}
	if certHash != "" {
		log.Printf("[wt] self-signed certificate, serverCertificateHashes sha-256 %s", certHash)
	}
	log.Printf("[wt] WebTransport %s (udp %s) -> native udp %s:%d (targets %s)", url, *addr, nativeHost, nativePort,
		envOr("KRUNK_NATIVE_TARGETS", "default"))

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		log.Printf("[wt] stopping (%d sessions)", r.sessions.Load())
		wt.Close()
	}()
	if err := wt.ListenAndServe(); err != nil && !strings.Contains(err.Error(), "closed") {
		log.Fatalf("[wt] %v", err)
	}
}
