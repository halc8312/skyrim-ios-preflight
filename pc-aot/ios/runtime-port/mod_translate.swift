// SPDX-License-Identifier: GPL-3.0-only
import SwiftUI
import Translation
import UIKit

/// Objective-C calls this bridge from the installer's viewDidAppear.
@MainActor
@objc(SKTranslator)
public final class SKTranslator: NSObject {
    @objc(translate:in:completion:)
    public static func translate(_ texts: [String], in parent: UIViewController,
                                 completion: @escaping ([String]?) -> Void) {
        guard !texts.isEmpty, parent.viewIfLoaded?.window != nil else {
            completion(nil)
            return
        }
        #if targetEnvironment(simulator)
        // Apple supplies the Translation API on the simulator but does not translate text there.
        completion(nil)
        #else
        let request = InstallerTranslation(completion)
        let view = InstallerTranslationView(texts: texts, request: request)
        let host = UIHostingController(rootView: view)
        request.host = host
        parent.addChild(host)
        host.view.frame = CGRect(x: 0, y: 0, width: 1, height: 1)
        host.view.backgroundColor = .clear
        parent.view.insertSubview(host.view, at: 0)
        host.didMove(toParent: parent)
        #endif
    }
}

@MainActor
private final class InstallerTranslation {
    weak var host: UIViewController?
    private var completion: (([String]?) -> Void)?

    init(_ completion: @escaping ([String]?) -> Void) {
        self.completion = completion
    }

    func finish(_ translated: [String]?) {
        guard let done = completion else { return }
        completion = nil
        if let host, host.parent != nil {
            host.willMove(toParent: nil)
            host.view.removeFromSuperview()
            host.removeFromParent()
        }
        done(translated)
    }
}

private struct InstallerTranslationView: View {
    let texts: [String]
    let request: InstallerTranslation

    var body: some View {
        Color.clear.translationTask(
            source: nil, target: Locale.Language(identifier: "ja"),
            action: Self.translate(texts, request: request))
    }

    // Keep the session on the task that owns it. Only its Sendable string results cross
    // to the main actor. Apple's batch API returns responses in the order of the requests.
    nonisolated private static func translate(_ texts: [String], request: InstallerTranslation)
        -> @Sendable (TranslationSession) async -> Void {
        { session in
            let batch = texts.map { TranslationSession.Request(sourceText: $0) }
            do {
                let responses = try await session.translations(from: batch)
                let values = responses.map { $0.targetText }
                let complete = values.count == texts.count && !values.contains("")
                await request.finish(complete ? values : nil)
            } catch {
                // This includes declining the system prompt to download language data.
                await request.finish(nil)
            }
        }
    }
}
