// RUN: %target-swift-frontend -emit-ir %s -swift-version 5 -enable-experimental-concurrency | %FileCheck -check-prefix CHECK -check-prefix CHECK-%target-ptrsize %s
// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation

// CHECK: %T16actor_class_objc7MyClassC = type <{ %swift.refcounted, [10 x i8*], %TSi }>

// CHECK: @"$s16actor_class_objc7MyClassCMm" = global
// CHECK-SAME: %objc_class* @"$s12_Concurrency12DefaultActorCMm"

// FIXME: It'd be nice if we could emit the metadata inline
// We'd have to permanently lock the set of methods in DefaultActor, though.
// CHECK: @"$s16actor_class_objc7MyClassCMP" = internal constant

public actor class MyClass: NSObject {
  public var x: Int
  public override init() { self.x = 0 }
}

// FIXME: it would be nice to inherit this
// CHECK-LABEL: define {{.*}}void @"$s16actor_class_objc7MyClassC7enqueue11partialTasky12_Concurrency012PartialAsyncG0V_tF"
// CHECK:      [[T0:%.*]] = bitcast %T16actor_class_objc7MyClassC* %1 to %objc_object*
// CHECK-NEXT: call swiftcc void @swift_defaultActor_enqueue(%swift.opaque* noalias nocapture %0, %objc_object* [[T0]])

// CHECK-LABEL: define {{.*}}i64 @"$s16actor_class_objc7MyClassC1xSivg"
// CHECK: [[T0:%.*]] = getelementptr inbounds %T16actor_class_objc7MyClassC, %T16actor_class_objc7MyClassC* %0, i32 0, i32 2
// CHECK: [[T1:%.*]] = getelementptr inbounds %TSi, %TSi* [[T0]], i32 0, i32 0
// CHECK-64: load i64, i64* [[T1]], align 16
// CHECK-32: load i32, i32* [[T1]], align 16

// CHECK-LABEL: define {{.*}}swiftcc %T16actor_class_objc7MyClassC* @"$s16actor_class_objc7MyClassCACycfc"
// FIXME: need to do this initialization!
// CHECK-NOT: swift_defaultActor_initialize
// CHECK-LABEL: ret %T16actor_class_objc7MyClassC*

// CHECK-LABEL: define {{.*}}swiftcc %swift.refcounted* @"$s16actor_class_objc7MyClassCfd"
// CHECK: call swiftcc %swift.refcounted* @"$s12_Concurrency12DefaultActorCfd"
