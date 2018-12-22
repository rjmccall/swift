// RUN: %target-swift-frontend -enable-sil-ownership -O -emit-sil -primary-file %s | %FileCheck %s

// CHECK-LABEL: sil hidden [transparent] @$s18subscript_accessor1XVxSgyciM
// CHECK: [[SETTER:%.*]] = function_ref @$s18subscript_accessor1XVxSgycis
// CHECK-NEXT: apply [[SETTER]]<T>
struct X<T> {
  subscript () -> T? {
    get {
      return nil
    }
    set { }
  }
}

  // CHECK: sil{{.*}}s18subscript_accessor9testXRead1xxAA1XVyxG_tlF
@_specialize(where T == (Int, Int))
func testXRead<T>(x: X<T>) -> T {
  return x[]!
}

// Don't crash dealing with T? in a non-generic context.
// rdar://44762116
struct WillBeConcretelyConstrained<T> {}
extension WillBeConcretelyConstrained where T == Int {
  subscript(key: Int) -> T? {
    get { return nil }
    set {}
  }

  subscript(key2 key: T) -> Int? {
    get { return nil }
    set {}
  }
}

// CHECK-LABEL: sil hidden [transparent] @$s18subscript_accessor27WillBeConcretelyConstrainedVAASiRszlEySiSgSiciM
// CHECK-SAME: $@yield_once @convention(method) (Int, @inout WillBeConcretelyConstrained<Int>) -> @yields @inout Optional<Int>

// CHECK-LABEL: sil hidden [transparent] @$s18subscript_accessor27WillBeConcretelyConstrainedVAASiRszlE4key2SiSgSi_tciM
// CHECK-SAME: $@yield_once @convention(method) (Int, @inout WillBeConcretelyConstrained<Int>) -> @yields @inout Optional<Int>


//   Check for specialization of testXRead.
// CHECK: $s18subscript_accessor1XVxSgycisTf4dn_n
