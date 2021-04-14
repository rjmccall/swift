// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-library-evolution -emit-module -emit-module-path=%t/helper.swiftmodule -module-name=helper %S/Inputs/resilient_final_helper.swift

// RUN: %target-swift-emit-silgen -I %t -module-name resilient_final %s | %FileCheck %s

import helper

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final15testPropertyGet3objy6helper8ExternalC_tF :
func testPropertyGet(obj: External) {
  // CHECK: class_method {{.*}} : $External, #External.property!getter
  _ = obj.property
}

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final15testPropertySet3objy6helper8ExternalC_tF :
func testPropertySet(obj: External) {
  // CHECK: class_method {{.*}} : $External, #External.property!setter
  obj.property = 5
}

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final10testMethod3objy6helper8ExternalC_tF :
func testMethod(obj: External) {
  // CHECK: class_method {{.*}} : $External, #External.method
  obj.method()
}

public class Internal {
  public init() {
    property = 0
  }

  @_resilientFinal
  public var property: Int

  @_resilientFinal
  public func method() {}
}

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final15testPropertyGet3objyAA8InternalC_tF :
func testPropertyGet(obj: Internal) {
  // CHECK: function_ref @$s15resilient_final8InternalC8propertySivg :
  _ = obj.property
}

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final15testPropertySet3objyAA8InternalC_tF :
func testPropertySet(obj: Internal) {
  // CHECK: function_ref @$s15resilient_final8InternalC8propertySivs :
  obj.property = 5
}

// CHECK-LABEL: sil hidden [ossa] @$s15resilient_final10testMethod3objyAA8InternalC_tF :
func testMethod(obj: Internal) {
  // CHECK: function_ref @$s15resilient_final8InternalC6methodyyF
  obj.method()
}

public protocol P {
  var property: Int { get set }
  func method()
}

extension External: P {}

// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s6helper8ExternalC15resilient_final1PA2dEP8propertySivgTW :
// CHECK:         class_method {{.*}} : $External, #External.property!getter
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s6helper8ExternalC15resilient_final1PA2dEP8propertySivsTW :
// CHECK:         class_method {{.*}} : $External, #External.property!setter
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s6helper8ExternalC15resilient_final1PA2dEP8propertySivMTW :
// CHECK:         class_method {{.*}} : $External, #External.property!modify
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s6helper8ExternalC15resilient_final1PA2dEP6methodyyFTW :
// CHECK:         class_method {{.*}} : $External, #External.method

extension Internal: P {}

// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s15resilient_final8InternalCAA1PA2aDP8propertySivgTW :
// CHECK:         function_ref @$s15resilient_final8InternalC8propertySivg
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s15resilient_final8InternalCAA1PA2aDP8propertySivsTW :
// CHECK:         function_ref @$s15resilient_final8InternalC8propertySivs
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s15resilient_final8InternalCAA1PA2aDP8propertySivMTW :
// CHECK:         function_ref @$s15resilient_final8InternalC8propertySivM
// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s15resilient_final8InternalCAA1PA2aDP6methodyyFTW :
// CHECK:         function_ref @$s15resilient_final8InternalC6methodyyF
