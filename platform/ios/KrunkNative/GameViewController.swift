import UIKit
import WebKit

/// Fullscreen landscape WKWebView. The page decides it runs on a phone from the user
/// agent and shows Krunker's touch controls (client/src/ui/ui_mobile.c); the
/// "KrunkNativeApp" user-agent token tells it the app already is fullscreen/landscape.
/// Controllers reach the page through WebKit's Gamepad API.
final class GameViewController: UIViewController, WKNavigationDelegate, WKUIDelegate {
    private var webView: WKWebView!
    private let server = LocalServer()

    private func config(_ key: String) -> String {
        (Bundle.main.object(forInfoDictionaryKey: key) as? String ?? "").trimmingCharacters(in: .whitespaces)
    }

    override func loadView() {
        let conf = WKWebViewConfiguration()
        conf.allowsInlineMediaPlayback = true
        conf.mediaTypesRequiringUserActionForPlayback = []
        conf.applicationNameForUserAgent = "Mobile KrunkNativeApp/1.0"
        conf.websiteDataStore = .default() // localStorage: settings and binds persist
        let web = WKWebView(frame: .zero, configuration: conf)
        web.navigationDelegate = self
        web.uiDelegate = self
        web.isOpaque = false
        web.backgroundColor = .black
        web.scrollView.isScrollEnabled = false
        web.scrollView.bounces = false
        web.scrollView.contentInsetAdjustmentBehavior = .never
        if #available(iOS 16.4, *) { web.isInspectable = true }
        webView = web
        view = web
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        startURL { [weak self] url in
            guard let self = self, let url = url else { return }
            self.webView.load(URLRequest(url: url))
        }
    }

    /// KNServerURL (open a hosted game), else the bundled client served on 127.0.0.1
    /// (a secure context, like the hosted page) with ?ws= or the offline server.
    private func startURL(_ done: @escaping (URL?) -> Void) {
        let remote = config("KNServerURL")
        if !remote.isEmpty { done(URL(string: remote)); return }
        guard let root = Bundle.main.resourceURL?.appendingPathComponent("web") else { done(nil); return }
        let assetBase = config("KNAssetBaseURL")
        server.start(root: root, assetBase: assetBase.isEmpty ? nil : assetBase) { port in
            guard let port = port else { done(nil); return }
            // KNStartPage: index.html, or strike/index.html in the Krunker Strike app
            let page = self.config("KNStartPage").isEmpty ? "index.html" : self.config("KNStartPage")
            var components = URLComponents(string: "http://127.0.0.1:\(port)/\(page)")!
            let ws = self.config("KNWebSocketURL")
            components.queryItems = [ws.isEmpty ? URLQueryItem(name: "offline", value: "1") : URLQueryItem(name: "ws", value: ws)]
            done(components.url)
        }
    }

    // Krunker's links (store pages, Discord, ...) open outside the app.
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = action.request.url, let host = url.host else { decisionHandler(.allow); return }
        let gameHost = webView.url?.host ?? host
        if host == gameHost || host == "127.0.0.1" || action.targetFrame?.isMainFrame == false {
            decisionHandler(.allow)
        } else {
            UIApplication.shared.open(url)
            decisionHandler(.cancel)
        }
    }

    // window.open(): external browser
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let url = action.request.url { UIApplication.shared.open(url) }
        return nil
    }

    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
}
