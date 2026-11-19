// SPDX-License-Identifier: GPL-3.0-only
// On-device translation of MOD installer text into Japanese with Apple's
// Translation framework (iOS 18+). Text never leaves the device; the first
// use may ask the user to download the language data. Objective-C looks this
// class up by name (NSClassFromString("SKTranslator")), so no generated Swift
// header is needed; if the class is absent the installer uses its glossary.
import SwiftUI
import Translation

@objc(SKTranslator)
public final class SKTranslator: NSObject {
    /// Translates `texts` (source language detected automatically) into
    /// Japanese. `completion` runs on the main thread with results in the same
    /// order, or nil when translation is unavailable or was declined.
    @objc(translate:in:completion:)
    public static func translate(_ texts: [String], in parent: UIViewController,
                                 completion: @escaping @MainActor ([String]?) -> Void) {
        guard #available(iOS 18.0, *), !texts.isEmpty else {
            Task { @MainActor in completion(nil) }
            return
        }
        Task { @MainActor in
            var host: UIHostingController<TranslatorView>?
            let view = TranslatorView(texts: texts) { result in
                host?.willMove(toParent: nil)
                host?.view.removeFromSuperview()
                host?.removeFromParent()
                host = nil
                completion(result)
            }
            let hosting = UIHostingController(rootView: view)
            host = hosting
            // A nearly invisible 1-pt view: the framework needs a view in a
            // window to show its own download prompt.
            hosting.view.frame = CGRect(x: 0, y: 0, width: 1, height: 1)
            hosting.view.alpha = 0.01
            hosting.view.isUserInteractionEnabled = false
            parent.addChild(hosting)
            parent.view.addSubview(hosting.view)
            hosting.didMove(toParent: parent)
        }
    }
}

@available(iOS 18.0, *)
struct TranslatorView: View {
    let texts: [String]
    let done: @MainActor ([String]?) -> Void
    @State private var configuration: TranslationSession.Configuration?

    var body: some View {
        let texts = self.texts
        let done = self.done
        Color.clear
            .onAppear {
                configuration = TranslationSession.Configuration(source: nil,
                                                                 target: Locale.Language(identifier: "ja"))
            }
            .translationTask(configuration) { session in
                let requests = texts.enumerated().map {
                    TranslationSession.Request(sourceText: $0.element, clientIdentifier: String($0.offset))
                }
                do {
                    let responses = try await session.translations(from: requests)
                    var out = texts
                    for response in responses {
                        if let id = response.clientIdentifier, let index = Int(id), index < out.count {
                            out[index] = response.targetText
                        }
                    }
                    let result = out
                    await MainActor.run { done(result) }
                } catch {
                    await MainActor.run { done(nil) }
                }
            }
    }
}
