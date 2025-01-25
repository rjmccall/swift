//===--- LinkContext.h - Context for linking computations -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2025 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_LINKCONTEXT_H
#define SWIFT_IRGEN_LINKCONTEXT_H

namespace llvm {
class Triple;
}

namespace swift {
namespace irgen {
class IRGenModule;

/// The emission context of a linkage computation.
class LinkContext {
public:
  ModuleDecl *SwiftModule;

  bool IsELFObject;
  bool IsMSVCEnvironment;
  bool UseDLLStorage;
  bool Internalize;

  /// True iff are multiple llvm modules.
  bool HasMultipleIGMs;

  /// When this is true, the linkage for forward-declared private symbols will
  /// be promoted to public external. Used by the LLDB expression evaluator.
  bool ForcePublicDecls;

  static LinkContext get(IRGenModule &IGM);

  LinkContext(ModuleDecl *swiftModule, const llvm::Triple &triple,
              bool hasMultipleIGMs,
              bool forcePublicDecls, bool isStaticLibrary);

  /// In case of multiple llvm modules (in multi-threaded compilation) all
  /// private decls must be visible from other files.
  bool shouldAllPrivateDeclsBeVisibleFromOtherFiles() const {
    return HasMultipleIGMs;
  }
  /// In case of multiple llvm modules, private lazy protocol
  /// witness table accessors could be emitted by two different IGMs during
  /// IRGen into different object files and the linker would complain about
  /// duplicate symbols.
  bool needLinkerToMergeDuplicateSymbols() const { return HasMultipleIGMs; }

  /// This is used by the LLDB expression evaluator since an expression's
  /// llvm::Module may need to access private symbols defined in the
  /// expression's context. This flag ensures that private accessors are
  /// forward-declared as public external in the expression's module.
  bool forcePublicDecls() const { return ForcePublicDecls; }
};

} // end namespace swift::irgen
} // end namespace swift

#endif
