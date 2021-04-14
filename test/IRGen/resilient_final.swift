// RUN: %target-swift-emit-ir -module-name resilient_final %s | %IRGenFileCheck %s

public class Internal {
  public init() {
    property = 0
  }

  public func first_method() {}

  @_resilientFinal
  public var property: Int

  @_resilientFinal
  public func method() {}

  public func last_method() {}
}

//   Test that we don't add v-table entries for the resilient_final methods
// CHECK-LABEL: @"$s15resilient_final8InternalCMf" =
// CHECK-SAME: @"$s15resilient_final8InternalC12first_methodyyF"
// CHECK-NOT:  @"$s15resilient_final8InternalC8propertySivgTj"
// CHECK-NOT:  @"$s15resilient_final8InternalC8propertySivsTj"
// CHECK-NOT:  @"$s15resilient_final8InternalC8propertySivMTj"
// CHECK-NOT:  @"$s15resilient_final8InternalC6methodyyFTj"
// CHECK-SAME: @"$s15resilient_final8InternalC11last_methodyyF"

//   Test that we emit dispatch thunks for the resilient_final methods
// CHECK-LABEL: define {{.*}} @"$s15resilient_final8InternalC8propertySivgTj"
// CHECK-LABEL: define {{.*}} @"$s15resilient_final8InternalC8propertySivsTj"
// CHECK-LABEL: define {{.*}} @"$s15resilient_final8InternalC8propertySivMTj"
// CHECK-LABEL: define {{.*}} @"$s15resilient_final8InternalC6methodyyFTj"
