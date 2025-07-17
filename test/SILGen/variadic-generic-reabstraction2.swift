// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

// rdar://110391963

func identity<each Parameter>(_ parameter: repeat each Parameter) -> (repeat each Parameter) {
    return (repeat each parameter)
}

[1, 2, 3].map(identity)



func test<each T>(tuple: (repeat each T, Int)) -> (repeat each T, Int?) {
  return tuple
}

func test2<each T>(tuple: (repeat each T)) -> (repeat each T?) {
  return tuple
}