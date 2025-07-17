// RUN: %target-swift-frontend -swift-version 6 -disable-availability-checking -emit-sil -o - %s -parse-as-library -verify -import-objc-header %S/Inputs/sending.h | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: asserts
// REQUIRES: objc_interop

// CHECK-LABEL: sil hidden @$s23sending_objc_convention18testConcreteResult1xySo6MyTypeC_tF :
func testConcreteResult(x: MyType) {
  //   Check that this returns an autoreleased result.
  // CHECK: objc_method %0, #MyType.getSendingResult!foreign : (MyType) -> () -> sending NSObject, $@convention(objc_method) (MyType) -> @sil_sending @autoreleased NSObject
  _ = x.getSendingResult()
}

// CHECK-LABEL: sil hidden @$s23sending_objc_convention18testProtocolResult1xySo7MyProto_p_tF :
func testProtocolResult(x: any MyProto) {
  //   Check that this returns an autoreleased result.
  // CHECK: [[SELF:%.*]] = open_existential_ref %0 to
  // CHECK: objc_method [[SELF]], #MyProto.produce!foreign : <Self where Self : MyProto> (Self) -> () -> sending NSObject, $@convention(objc_method) <τ_0_0 where τ_0_0 : MyProto> (τ_0_0) -> @sil_sending @autoreleased NSObject
  _ = x.produce()  
}

// CHECK-LABEL: sil hidden @$s23sending_objc_convention17testConcreteParam1xySo6MyTypeC_tF :
func testConcreteParam(x: MyType) {
  let s = NSObject()

  //   Check that this takes an unowned parameter.
  // CHECK: objc_method %0, #MyType.getResultWithSendingArgument!foreign : (MyType) -> (sending NSObject) -> NSObject, $@convention(objc_method) (@sil_sending NSObject, MyType) -> @autoreleased NSObject
  _ = x.getResultWithSendingArgument(s)
}

// CHECK-LABEL: sil hidden @$s23sending_objc_convention17testProtocolParam1xySo7MyProto_p_tF :
func testProtocolParam(x: any MyProto) {
  let s = NSObject()

  //   Check that this takes an unowned parameter.
  // CHECK: [[SELF:%.*]] = open_existential_ref %0 to
  // CHECK: objc_method [[SELF]], #MyProto.receive!foreign : <Self where Self : MyProto> (Self) -> (sending NSObject) -> (), $@convention(objc_method) <τ_0_0 where τ_0_0 : MyProto> (@sil_sending NSObject, τ_0_0) -> ()
  _ = x.receive(s)
}

// CHECK-LABEL: sil hidden @$s23sending_objc_convention25testOptionalProtocolParam1xySo7MyProto_p_tF :
func testOptionalProtocolParam(x: any MyProto) {
  let s = NSObject()

  //   Check that this takes an unowned parameter.
  //   It seems arbitrary that we produce an objc_method with a substituted
  //   type on the optional method but not the concrete protocol method, but
  //   it has nothing to do with `sending`.
  // CHECK: [[SELF:%.*]] = open_existential_ref %0 to $[[OPENED_TYPE:@opened\(".*", any MyProto\) Self]]
  // CHECK: objc_method [[SELF]], #MyProto.optionalReceive!foreign : <Self where Self : MyProto> (Self) -> (sending NSObject) -> (), $@convention(objc_method) (@sil_sending NSObject, [[OPENED_TYPE]]) -> ()
  _ = x.optionalReceive!(s)
}

class MyOverrider : MyType {
  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyOverriderC16getSendingResultSo8NSObjectCyFTo :
  override func getSendingResult() -> sending NSObject {
    //   Check that this returns an autoreleased result.
    // CHECK-SAME:    $@convention(objc_method) (MyOverrider) -> @sil_sending @autoreleased NSObject
    return super.getSendingResult()
  }

  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyOverriderC28getResultWithSendingArgumentySo8NSObjectCAFnFTo :
  override func getResultWithSendingArgument(_ s: sending NSObject) -> NSObject {
    //   Check that this takes an unowned parameter.
    // CHECK-SAME:    $@convention(objc_method) (@sil_sending NSObject, MyOverrider) -> ()
    return super.getResultWithSendingArgument(s)
  }

  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyOverriderC16getSendingResult12withArgumentSo8NSObjectCAG_tFTo :
  override func getSendingResult(withArgument s: NSObject) -> sending NSObject {
    //   Check that this takes an unowned parameter.
    // CHECK-SAME: $@convention(objc_method) (@sil_sending NSObject, MyOverrider) -> ()
    return super.getSendingResult(withArgument: s)
  }
}

class MyConformer : NSObject, MyProto {
  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyConformerC7produceSo8NSObjectCyFTo :
  func produce() -> sending NSObject {
    //   Check that this returns an autoreleased result.
    // CHECK-SAME:    $@convention(objc_method) (MyConformer) -> @sil_sending @autoreleased NSObject
    return NSObject()
  }

  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyConformerC7receiveyySo8NSObjectCnFTo :
  func receive(_: sending NSObject) {
    //   Check that this takes an unowned parameter.
    // CHECK-SAME:    $@convention(objc_method) (@sil_sending NSObject, MyConformer) -> ()
  }

  // CHECK-LABEL: sil private [thunk] @$s23sending_objc_convention11MyConformerC15optionalReceiveyySo8NSObjectCnFTo :
  func optionalReceive(_: sending NSObject) {
    //   Check that this takes an unowned parameter.
    // CHECK-SAME: $@convention(objc_method) (@sil_sending NSObject, MyConformer) -> ()
  }
}
