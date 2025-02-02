//===- CastValueChecker - Model implementation of custom RTTIs --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This defines CastValueChecker which models casts of custom RTTIs.
//
// TODO list:
// - It only allows one succesful cast between two types however in the wild
//   the object could be casted to multiple types.
// - It needs to check the most likely type information from the dynamic type
//   map to increase precision of dynamic casting.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/DeclTemplate.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicType.h"
#include "llvm/ADT/Optional.h"
#include <utility>

using namespace clang;
using namespace ento;

namespace {
class CastValueChecker : public Checker<eval::Call> {
  enum class CallKind { Function, Method, InstanceOf };

  using CastCheck =
      std::function<void(const CastValueChecker *, const CallEvent &Call,
                         DefinedOrUnknownSVal, CheckerContext &)>;

public:
  // We have five cases to evaluate a cast:
  // 1) The parameter is non-null, the return value is non-null.
  // 2) The parameter is non-null, the return value is null.
  // 3) The parameter is null, the return value is null.
  // cast: 1;  dyn_cast: 1, 2;  cast_or_null: 1, 3;  dyn_cast_or_null: 1, 2, 3.
  //
  // 4) castAs: Has no parameter, the return value is non-null.
  // 5) getAs:  Has no parameter, the return value is null or non-null.
  //
  // We have two cases to check the parameter is an instance of the given type.
  // 1) isa:             The parameter is non-null, returns boolean.
  // 2) isa_and_nonnull: The parameter is null or non-null, returns boolean.
  bool evalCall(const CallEvent &Call, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SR, CheckerContext &C) const;

private:
  // These are known in the LLVM project. The pairs are in the following form:
  // {{{namespace, call}, argument-count}, {callback, kind}}
  const CallDescriptionMap<std::pair<CastCheck, CallKind>> CDM = {
      {{{"llvm", "cast"}, 1},
       {&CastValueChecker::evalCast, CallKind::Function}},
      {{{"llvm", "dyn_cast"}, 1},
       {&CastValueChecker::evalDynCast, CallKind::Function}},
      {{{"llvm", "cast_or_null"}, 1},
       {&CastValueChecker::evalCastOrNull, CallKind::Function}},
      {{{"llvm", "dyn_cast_or_null"}, 1},
       {&CastValueChecker::evalDynCastOrNull, CallKind::Function}},
      {{{"clang", "castAs"}, 0},
       {&CastValueChecker::evalCastAs, CallKind::Method}},
      {{{"clang", "getAs"}, 0},
       {&CastValueChecker::evalGetAs, CallKind::Method}},
      {{{"llvm", "isa"}, 1},
       {&CastValueChecker::evalIsa, CallKind::InstanceOf}},
      {{{"llvm", "isa_and_nonnull"}, 1},
       {&CastValueChecker::evalIsaAndNonNull, CallKind::InstanceOf}}};

  void evalCast(const CallEvent &Call, DefinedOrUnknownSVal DV,
                CheckerContext &C) const;
  void evalDynCast(const CallEvent &Call, DefinedOrUnknownSVal DV,
                   CheckerContext &C) const;
  void evalCastOrNull(const CallEvent &Call, DefinedOrUnknownSVal DV,
                      CheckerContext &C) const;
  void evalDynCastOrNull(const CallEvent &Call, DefinedOrUnknownSVal DV,
                         CheckerContext &C) const;
  void evalCastAs(const CallEvent &Call, DefinedOrUnknownSVal DV,
                  CheckerContext &C) const;
  void evalGetAs(const CallEvent &Call, DefinedOrUnknownSVal DV,
                 CheckerContext &C) const;
  void evalIsa(const CallEvent &Call, DefinedOrUnknownSVal DV,
               CheckerContext &C) const;
  void evalIsaAndNonNull(const CallEvent &Call, DefinedOrUnknownSVal DV,
                         CheckerContext &C) const;
};
} // namespace

static QualType getRecordType(QualType Ty) {
  Ty = Ty.getCanonicalType();

  if (Ty->isPointerType())
    Ty = Ty->getPointeeType();

  if (Ty->isReferenceType())
    Ty = Ty.getNonReferenceType();

  return Ty.getUnqualifiedType();
}

static bool isInfeasibleCast(const DynamicCastInfo *CastInfo,
                             bool CastSucceeds) {
  if (!CastInfo)
    return false;

  return CastSucceeds ? CastInfo->fails() : CastInfo->succeeds();
}

static const NoteTag *getNoteTag(CheckerContext &C,
                                 const DynamicCastInfo *CastInfo,
                                 QualType CastToTy, const Expr *Object,
                                 bool CastSucceeds, bool IsKnownCast) {
  std::string CastToName =
      CastInfo ? CastInfo->to()->getAsCXXRecordDecl()->getNameAsString()
               : CastToTy->getAsCXXRecordDecl()->getNameAsString();
  Object = Object->IgnoreParenImpCasts();

  return C.getNoteTag(
      [=]() -> std::string {
        SmallString<128> Msg;
        llvm::raw_svector_ostream Out(Msg);

        if (!IsKnownCast)
          Out << "Assuming ";

        if (const auto *DRE = dyn_cast<DeclRefExpr>(Object)) {
          Out << '\'' << DRE->getDecl()->getNameAsString() << '\'';
        } else if (const auto *ME = dyn_cast<MemberExpr>(Object)) {
          Out << (IsKnownCast ? "Field '" : "field '")
              << ME->getMemberDecl()->getNameAsString() << '\'';
        } else {
          Out << (IsKnownCast ? "The object" : "the object");
        }

        Out << ' ' << (CastSucceeds ? "is a" : "is not a") << " '" << CastToName
            << '\'';

        return Out.str();
      },
      /*IsPrunable=*/true);
}

//===----------------------------------------------------------------------===//
// Main logic to evaluate a cast.
//===----------------------------------------------------------------------===//

static void addCastTransition(const CallEvent &Call, DefinedOrUnknownSVal DV,
                              CheckerContext &C, bool IsNonNullParam,
                              bool IsNonNullReturn,
                              bool IsCheckedCast = false) {
  ProgramStateRef State = C.getState()->assume(DV, IsNonNullParam);
  if (!State)
    return;

  const Expr *Object;
  QualType CastFromTy;
  QualType CastToTy = getRecordType(Call.getResultType());

  if (Call.getNumArgs() > 0) {
    Object = Call.getArgExpr(0);
    CastFromTy = getRecordType(Call.parameters()[0]->getType());
  } else {
    Object = cast<CXXInstanceCall>(&Call)->getCXXThisExpr();
    CastFromTy = getRecordType(Object->getType());
  }

  const MemRegion *MR = DV.getAsRegion();
  const DynamicCastInfo *CastInfo =
      getDynamicCastInfo(State, MR, CastFromTy, CastToTy);

  // We assume that every checked cast succeeds.
  bool CastSucceeds = IsCheckedCast || CastFromTy == CastToTy;
  if (!CastSucceeds) {
    if (CastInfo)
      CastSucceeds = IsNonNullReturn && CastInfo->succeeds();
    else
      CastSucceeds = IsNonNullReturn;
  }

  // Check for infeasible casts.
  if (isInfeasibleCast(CastInfo, CastSucceeds)) {
    C.generateSink(State, C.getPredecessor());
    return;
  }

  // Store the type and the cast information.
  bool IsKnownCast = CastInfo || IsCheckedCast || CastFromTy == CastToTy;
  if (!IsKnownCast || IsCheckedCast)
    State = setDynamicTypeAndCastInfo(State, MR, CastFromTy, CastToTy,
                                      Call.getResultType(), CastSucceeds);

  SVal V = CastSucceeds ? DV : C.getSValBuilder().makeNull();
  C.addTransition(
      State->BindExpr(Call.getOriginExpr(), C.getLocationContext(), V, false),
      getNoteTag(C, CastInfo, CastToTy, Object, CastSucceeds, IsKnownCast));
}

static void addInstanceOfTransition(const CallEvent &Call,
                                    DefinedOrUnknownSVal DV,
                                    ProgramStateRef State, CheckerContext &C,
                                    bool IsInstanceOf) {
  const FunctionDecl *FD = Call.getDecl()->getAsFunction();
  QualType CastToTy = FD->getTemplateSpecializationArgs()->get(0).getAsType();
  QualType CastFromTy = getRecordType(Call.parameters()[0]->getType());

  const MemRegion *MR = DV.getAsRegion();
  const DynamicCastInfo *CastInfo =
      getDynamicCastInfo(State, MR, CastFromTy, CastToTy);

  bool CastSucceeds;
  if (CastInfo)
    CastSucceeds = IsInstanceOf && CastInfo->succeeds();
  else
    CastSucceeds = IsInstanceOf || CastFromTy == CastToTy;

  if (isInfeasibleCast(CastInfo, CastSucceeds)) {
    C.generateSink(State, C.getPredecessor());
    return;
  }

  // Store the type and the cast information.
  bool IsKnownCast = CastInfo || CastFromTy == CastToTy;
  if (!IsKnownCast)
    State = setDynamicTypeAndCastInfo(State, MR, CastFromTy, CastToTy,
                                      Call.getResultType(), IsInstanceOf);

  C.addTransition(
      State->BindExpr(Call.getOriginExpr(), C.getLocationContext(),
                      C.getSValBuilder().makeTruthVal(CastSucceeds)),
      getNoteTag(C, CastInfo, CastToTy, Call.getArgExpr(0), CastSucceeds,
                 IsKnownCast));
}

//===----------------------------------------------------------------------===//
// Evaluating cast, dyn_cast, cast_or_null, dyn_cast_or_null.
//===----------------------------------------------------------------------===//

static void evalNonNullParamNonNullReturn(const CallEvent &Call,
                                          DefinedOrUnknownSVal DV,
                                          CheckerContext &C,
                                          bool IsCheckedCast = false) {
  addCastTransition(Call, DV, C, /*IsNonNullParam=*/true,
                    /*IsNonNullReturn=*/true, IsCheckedCast);
}

static void evalNonNullParamNullReturn(const CallEvent &Call,
                                       DefinedOrUnknownSVal DV,
                                       CheckerContext &C) {
  addCastTransition(Call, DV, C, /*IsNonNullParam=*/true,
                    /*IsNonNullReturn=*/false);
}

static void evalNullParamNullReturn(const CallEvent &Call,
                                    DefinedOrUnknownSVal DV,
                                    CheckerContext &C) {
  if (ProgramStateRef State = C.getState()->assume(DV, false))
    C.addTransition(State->BindExpr(Call.getOriginExpr(),
                                    C.getLocationContext(),
                                    C.getSValBuilder().makeNull(), false),
                    C.getNoteTag("Assuming null pointer is passed into cast",
                                 /*IsPrunable=*/true));
}

void CastValueChecker::evalCast(const CallEvent &Call, DefinedOrUnknownSVal DV,
                                CheckerContext &C) const {
  evalNonNullParamNonNullReturn(Call, DV, C, /*IsCheckedCast=*/true);
}

void CastValueChecker::evalDynCast(const CallEvent &Call,
                                   DefinedOrUnknownSVal DV,
                                   CheckerContext &C) const {
  evalNonNullParamNonNullReturn(Call, DV, C);
  evalNonNullParamNullReturn(Call, DV, C);
}

void CastValueChecker::evalCastOrNull(const CallEvent &Call,
                                      DefinedOrUnknownSVal DV,
                                      CheckerContext &C) const {
  evalNonNullParamNonNullReturn(Call, DV, C);
  evalNullParamNullReturn(Call, DV, C);
}

void CastValueChecker::evalDynCastOrNull(const CallEvent &Call,
                                         DefinedOrUnknownSVal DV,
                                         CheckerContext &C) const {
  evalNonNullParamNonNullReturn(Call, DV, C);
  evalNonNullParamNullReturn(Call, DV, C);
  evalNullParamNullReturn(Call, DV, C);
}

//===----------------------------------------------------------------------===//
// Evaluating castAs, getAs.
//===----------------------------------------------------------------------===//

static void evalZeroParamNonNullReturn(const CallEvent &Call,
                                       DefinedOrUnknownSVal DV,
                                       CheckerContext &C,
                                       bool IsCheckedCast = false) {
  addCastTransition(Call, DV, C, /*IsNonNullParam=*/true,
                    /*IsNonNullReturn=*/true, IsCheckedCast);
}

static void evalZeroParamNullReturn(const CallEvent &Call,
                                    DefinedOrUnknownSVal DV,
                                    CheckerContext &C) {
  addCastTransition(Call, DV, C, /*IsNonNullParam=*/true,
                    /*IsNonNullReturn=*/false);
}

void CastValueChecker::evalCastAs(const CallEvent &Call,
                                  DefinedOrUnknownSVal DV,
                                  CheckerContext &C) const {
  evalZeroParamNonNullReturn(Call, DV, C, /*IsCheckedCast=*/true);
}

void CastValueChecker::evalGetAs(const CallEvent &Call, DefinedOrUnknownSVal DV,
                                 CheckerContext &C) const {
  evalZeroParamNonNullReturn(Call, DV, C);
  evalZeroParamNullReturn(Call, DV, C);
}

//===----------------------------------------------------------------------===//
// Evaluating isa, isa_and_nonnull.
//===----------------------------------------------------------------------===//

void CastValueChecker::evalIsa(const CallEvent &Call, DefinedOrUnknownSVal DV,
                               CheckerContext &C) const {
  ProgramStateRef NonNullState, NullState;
  std::tie(NonNullState, NullState) = C.getState()->assume(DV);

  if (NonNullState) {
    addInstanceOfTransition(Call, DV, NonNullState, C, /*IsInstanceOf=*/true);
    addInstanceOfTransition(Call, DV, NonNullState, C, /*IsInstanceOf=*/false);
  }

  if (NullState) {
    C.generateSink(NullState, C.getPredecessor());
  }
}

void CastValueChecker::evalIsaAndNonNull(const CallEvent &Call,
                                         DefinedOrUnknownSVal DV,
                                         CheckerContext &C) const {
  ProgramStateRef NonNullState, NullState;
  std::tie(NonNullState, NullState) = C.getState()->assume(DV);

  if (NonNullState) {
    addInstanceOfTransition(Call, DV, NonNullState, C, /*IsInstanceOf=*/true);
    addInstanceOfTransition(Call, DV, NonNullState, C, /*IsInstanceOf=*/false);
  }

  if (NullState) {
    addInstanceOfTransition(Call, DV, NullState, C, /*IsInstanceOf=*/false);
  }
}

//===----------------------------------------------------------------------===//
// Main logic to evaluate a call.
//===----------------------------------------------------------------------===//

bool CastValueChecker::evalCall(const CallEvent &Call,
                                CheckerContext &C) const {
  const auto *Lookup = CDM.lookup(Call);
  if (!Lookup)
    return false;

  const CastCheck &Check = Lookup->first;
  CallKind Kind = Lookup->second;

  // We need to obtain the record type of the call's result to model it.
  if (Kind != CallKind::InstanceOf &&
      !getRecordType(Call.getResultType())->isRecordType())
    return false;

  Optional<DefinedOrUnknownSVal> DV;

  switch (Kind) {
  case CallKind::Function: {
    // We need to obtain the record type of the call's parameter to model it.
    if (!getRecordType(Call.parameters()[0]->getType())->isRecordType())
      return false;

    DV = Call.getArgSVal(0).getAs<DefinedOrUnknownSVal>();
    break;
  }
  case CallKind::InstanceOf: {
    // We need to obtain the only template argument to determinte the type.
    const FunctionDecl *FD = Call.getDecl()->getAsFunction();
    if (!FD || !FD->getTemplateSpecializationArgs())
      return false;

    DV = Call.getArgSVal(0).getAs<DefinedOrUnknownSVal>();
    break;
  }
  case CallKind::Method:
    const auto *InstanceCall = dyn_cast<CXXInstanceCall>(&Call);
    if (!InstanceCall)
      return false;

    DV = InstanceCall->getCXXThisVal().getAs<DefinedOrUnknownSVal>();
    break;
  }

  if (!DV)
    return false;

  Check(this, Call, *DV, C);
  return true;
}

void CastValueChecker::checkDeadSymbols(SymbolReaper &SR,
                                        CheckerContext &C) const {
  C.addTransition(removeDeadCasts(C.getState(), SR));
}

void ento::registerCastValueChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<CastValueChecker>();
}

bool ento::shouldRegisterCastValueChecker(const LangOptions &LO) {
  return true;
}
