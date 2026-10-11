import Foundation
import Network

/// Minimal static HTTP server on 127.0.0.1 for the bundled web client. The page needs
/// an http(s) origin (ui/js/loader.js only boots the wasm there; localStorage, fetch and
/// WebAssembly streaming behave like the hosted site). Requests for /assets/* that are
/// not in the bundle are redirected to KNAssetBaseURL when configured.
final class LocalServer {
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "io.krunknative.localserver")
    private var root = URL(fileURLWithPath: "/")
    private var assetBase: String?

    func start(root: URL, assetBase: String?, ready: @escaping (UInt16?) -> Void) {
        self.root = root.standardizedFileURL
        self.assetBase = assetBase
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        guard let listener = try? NWListener(using: params) else { ready(nil); return }
        self.listener = listener
        listener.newConnectionHandler = { [weak self] conn in self?.serve(conn) }
        listener.stateUpdateHandler = { state in
            switch state {
            case .ready: DispatchQueue.main.async { ready(listener.port?.rawValue) }
            case .failed: DispatchQueue.main.async { ready(nil) }
            default: break
            }
        }
        listener.start(queue: queue)
    }

    private func serve(_ conn: NWConnection) {
        conn.start(queue: queue)
        receiveRequest(conn, buffer: Data())
    }

    private func receiveRequest(_ conn: NWConnection, buffer: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
            guard let self = self else { return }
            var buf = buffer
            if let data = data { buf.append(data) }
            if let end = buf.range(of: Data("\r\n\r\n".utf8)) {
                let head = String(decoding: buf[..<end.lowerBound], as: UTF8.self)
                self.respond(conn, head: head)
            } else if done || error != nil || buf.count > 1 << 20 {
                conn.cancel()
            } else {
                self.receiveRequest(conn, buffer: buf)
            }
        }
    }

    private func respond(_ conn: NWConnection, head: String) {
        let requestLine = head.components(separatedBy: "\r\n").first ?? ""
        let parts = requestLine.split(separator: " ")
        guard parts.count >= 2, parts[0] == "GET" || parts[0] == "HEAD" else {
            send(conn, status: "405 Method Not Allowed", type: "text/plain", body: Data("method not allowed".utf8))
            return
        }
        var path = String(parts[1].split(separator: "?", maxSplits: 1).first ?? "/")
        path = path.removingPercentEncoding ?? path
        if path.hasSuffix("/") { path += "index.html" }
        let file = root.appendingPathComponent(String(path.drop(while: { $0 == "/" }))).standardizedFileURL
        guard file.path.hasPrefix(root.path) else {
            send(conn, status: "403 Forbidden", type: "text/plain", body: Data())
            return
        }
        if let body = try? Data(contentsOf: file) {
            send(conn, status: "200 OK", type: LocalServer.mimeType(file.pathExtension), body: parts[0] == "HEAD" ? Data() : body,
                 length: body.count)
        } else if path.hasPrefix("/assets/"), let base = assetBase {
            let target = (base.hasSuffix("/") ? String(base.dropLast()) : base) + path
            send(conn, status: "302 Found", type: "text/plain", body: Data(), extra: "Location: \(target)\r\n")
        } else {
            send(conn, status: "404 Not Found", type: "text/plain", body: Data("not found".utf8))
        }
    }

    private func send(_ conn: NWConnection, status: String, type: String, body: Data, length: Int? = nil, extra: String = "") {
        var header = "HTTP/1.1 \(status)\r\nContent-Type: \(type)\r\nContent-Length: \(length ?? body.count)\r\n"
        header += "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\(extra)\r\n"
        var out = Data(header.utf8)
        out.append(body)
        conn.send(content: out, completion: .contentProcessed { _ in conn.cancel() })
    }

    static func mimeType(_ ext: String) -> String {
        switch ext.lowercased() {
        case "html": return "text/html; charset=utf-8"
        case "js": return "text/javascript"
        case "css": return "text/css"
        case "json": return "application/json"
        case "wasm": return "application/wasm"
        case "png": return "image/png"
        case "jpg", "jpeg": return "image/jpeg"
        case "gif": return "image/gif"
        case "webp": return "image/webp"
        case "svg": return "image/svg+xml"
        case "ico": return "image/x-icon"
        case "ttf": return "font/ttf"
        case "otf": return "font/otf"
        case "woff": return "font/woff"
        case "woff2": return "font/woff2"
        case "mp3": return "audio/mpeg"
        case "ogg": return "audio/ogg"
        case "wav": return "audio/wav"
        default: return "application/octet-stream"
        }
    }
}
