//===--- BuilderTransform.h - Function builders -----------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the BuilderTransform class, which represents the
// transformation to apply to a function due to a function builder.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_BUILDER_TRANSFORM_H
#define SWIFT_SEMA_BUILDER_TRANSFORM_H

#include "swift/AST/Type.h"

namespace swift {
class AnyFunctionRef;
class BraceStmt;
class Expr;

namespace constraints {
class ConstraintSystem;
class Solution;

/// The un-typechecked transformation applied to a function by a
/// function builder.
class BuilderTransform {
  Expr *ResultExpr;
  Type ResultType;

public:
  BuilderTransform(Expr *resultExpr, Type resultType)
    : ResultExpr(resultExpr), ResultType(resultType) {}

  /// Apply the given constraint-system solution to the body of the function
  /// from which it was originally derived and create a new body.
  bool applySolution(ConstraintSystem &cs, const Solution &solution,
                     AnyFunctionRef fn, Type returnType,
                     bool skipClosures) const;

  bool operator==(const BuilderTransform &other) const {
    return ResultExpr == other.ResultExpr;
  }

  Expr *getResultExpr() const {
    return ResultExpr;
  }
};

} // end namespace constraints
} // end namespace swift

#endif
