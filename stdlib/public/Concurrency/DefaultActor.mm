//===--- DefaultActor.mm - NSObject-inheriting DefaultActor support -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Define the __SwiftNativeNSDefaultActorBase class, which inherits from
// NSObject but uses Swift reference-counting.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#include <objc/NSObject.h>
#include <objc/runtime.h>
#include <objc/objc.h>
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/ObjCBridge.h"

using namespace swift;

SWIFT_RUNTIME_STDLIB_API
@interface __SwiftNSObjectDefaultActorBase : NSObject
{
@private
  SWIFT_HEAPOBJECT_NON_OBJC_MEMBERS;
}
@end

@implementation __SwiftNSObjectDefaultActorBase

- (id)initWithCoder: (NSCoder *)coder {
  return [super init];
}

+ (BOOL)automaticallyNotifiesObserversForKey:(NSString *)key {
  return NO;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"

STANDARD_OBJC_METHOD_IMPLS_FOR_SWIFT_OBJECTS

#pragma clang diagnostic pop

@end

#endif
