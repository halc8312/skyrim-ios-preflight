import SwiftUI

// This isolated app deliberately does not initialize Rust or a Kernel.
// Its single XCTest invokes the one-shot public probe in this process.
@main
struct ResidentAppleApp: App {
    var body: some Scene {
        WindowGroup {
            Text("Public resident integration probe")
                .padding()
        }
    }
}
