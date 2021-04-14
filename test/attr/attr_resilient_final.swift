// RUN: %target-typecheck-verify-swift

// expected-error@+1 {{only members of a primary class definition may be marked with '@_resilientFinal'}}
@_resilientFinal
var x = 0

// expected-error@+1 {{'@_resilientFinal' attribute cannot be applied to this declaration}}
@_resilientFinal
struct S1 {}

struct S2 {
  // expected-error@+1 {{only members of a primary class definition may be marked with '@_resilientFinal'}}
  @_resilientFinal
  func foo() {}
}

// For now, the attribute is only allowed on individual methods.
// expected-error@+1 {{'@_resilientFinal' attribute cannot be applied to this declaration}}
@_resilientFinal
class C0 {}

class C1 {}

// expected-error@+1 {{'@_resilientFinal' attribute cannot be applied to this declaration}}
@_resilientFinal
extension C1 {}

extension C1 {
  // expected-error@+1 {{only members of a primary class definition may be marked with '@_resilientFinal'}}
  @_resilientFinal
  func foo () {}
}

class Combo {
  // expected-error@+1 {{instance method cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  final func foo() {}

  // expected-error@+1 {{property cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  final var x: Int { return 0 }

  // expected-error@+1 {{subscript cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  final subscript(x: Int) -> Int { return 0}
}

final class ComboClassLevel {
  // expected-error@+1 {{instance method cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  func foo() {}

  // expected-error@+1 {{property cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  var x: Int { return 0 }

  // expected-error@+1 {{subscript cannot be both 'final' and '@_resilientFinal'}}
  @_resilientFinal
  subscript(x: Int) -> Int { return 0}
}

class SuperWithFinal {
  // expected-note@+2 {{overridden declaration is here}}
  @_resilientFinal
  func foo() {}

  // expected-note@+2 {{overridden declaration is here}}
  @_resilientFinal
  var x: Int { return 0 }

  // expected-note@+2 {{overridden declaration is here}}
  @_resilientFinal
  subscript(x: Int) -> Int { return 0}
}

class DerivedFromFinal: SuperWithFinal {
  // expected-error@+1 {{instance method overrides a 'final' instance method}}
  override func foo() {}

  // expected-error@+1 {{property overrides a 'final' property}}
  override var x: Int { return 1 }

  // expected-error@+1 {{subscript overrides a 'final' subscript}}
  override subscript(x: Int) -> Int { return 1 }
}
