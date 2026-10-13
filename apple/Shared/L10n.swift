import Foundation

/// Turkish on Turkish systems, English everywhere else.
let isTurkish = Locale.preferredLanguages.first?.hasPrefix("tr") ?? false

func tr(_ turkish: String, _ english: String) -> String {
    isTurkish ? turkish : english
}
