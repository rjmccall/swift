// RUN: %target-swift-frontend -module-name test -target %target-swift-5.1-abi-triple -swift-version 5 -sil-verify-all -emit-sil %s | %FileCheck --enable-var-scope --implicit-check-not='hop_to_executor' %s

// REQUIRES: concurrency
// REQUIRES: swift_in_compiler
// REQUIRES: objc_interop

import Foundation

// rdar://131095718
public actor A: NSObject {
  public init(_: String) async throws {}
}
