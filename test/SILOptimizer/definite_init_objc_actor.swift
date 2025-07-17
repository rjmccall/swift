// RUN: %target-swift-frontend -module-name test -target %target-swift-5.1-abi-triple -swift-version 5 -sil-verify-all -Xllvm -sil-print-types -emit-sil %s | %FileCheck --enable-var-scope --implicit-check-not='hop_to_executor' %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation

// rdar://131095718 - non-delegating isolated async initializer crashes on
// actor that inherits from NSObject
actor InheritsNSObject: NSObject {
  init(_: String) async throws {}
}
