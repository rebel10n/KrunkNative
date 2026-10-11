import UIKit

/// KrunkNative for iOS: Krunker's mobile apps were a fullscreen landscape web view
/// around the web game; this wraps the KrunkNative web client (client/src + ui/, built
/// with `make web`) the same way. See platform/README.md.
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = GameViewController()
        window.backgroundColor = .black
        window.makeKeyAndVisible()
        self.window = window
        UIApplication.shared.isIdleTimerDisabled = true // keep the screen on while playing
        return true
    }
}
