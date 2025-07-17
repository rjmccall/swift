// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

// rdar://109904746

func foo<T>(_ makeT: () throws -> T = { throw Nope() }) rethrows -> T {
    try makeT()
}

func bar<each T>() throws -> (repeat each T) {
    try foo() // compiler crash :(
}