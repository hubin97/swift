//===--- GenericSignature.cpp - Generic Signature AST ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the GenericSignature class.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
using namespace swift;

ASTContext &GenericSignature::getASTContext(
                                ArrayRef<swift::GenericTypeParamType *> params,
                                ArrayRef<swift::Requirement> requirements) {
  // The params and requirements cannot both be empty.
  if (!params.empty())
    return params.front()->getASTContext();
  else
    return requirements.front().getFirstType()->getASTContext();
}

CanGenericSignature
GenericSignature::getCanonicalSignature() const {
  if (CanonicalSignatureOrASTContext.is<ASTContext*>())
    // TODO: CanGenericSignature should be const-correct.
    return CanGenericSignature(const_cast<GenericSignature*>(this));
  
  if (auto p = CanonicalSignatureOrASTContext.dyn_cast<GenericSignature*>())
    return CanGenericSignature(p);
  
  CanGenericSignature canSig = getCanonical(getGenericParams(),
                                            getRequirements());
  if (canSig != this)
    CanonicalSignatureOrASTContext = canSig;
  return canSig;
}

/// Canonical ordering for dependent types in generic signatures.
static int compareDependentTypes(const CanType *pa, const CanType *pb) {
  auto a = *pa, b = *pb;
  
  // Fast-path check for equality.
  if (a == b)
    return 0;

  // Ordering is as follows:
  // - Generic params
  if (auto gpa = dyn_cast<GenericTypeParamType>(a)) {
    if (auto gpb = dyn_cast<GenericTypeParamType>(b)) {
      // - by depth, so t_0_n < t_1_m
      if (int compareDepth = gpa->getDepth() - gpb->getDepth())
        return compareDepth;
      // - by index, so t_n_0 < t_n_1
      return gpa->getIndex() - gpb->getIndex();
    }
    return -1;
  }
  
  // - Dependent members
  if (auto dma = dyn_cast<DependentMemberType>(a)) {
    if (isa<GenericTypeParamType>(b))
      return +1;
    if (auto dmb = dyn_cast<DependentMemberType>(b)) {
      // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
      auto abase = dma.getBase();
      auto bbase = dmb.getBase();
      if (int compareBases = compareDependentTypes(&abase, &bbase))
        return compareBases;
      
      // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
      auto protoa = dma->getAssocType()->getProtocol();
      auto protob = dmb->getAssocType()->getProtocol();
      if (int compareProtocols
            = ProtocolType::compareProtocols(&protoa, &protob))
        return compareProtocols;
      
      // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
      return dma->getAssocType()->getName().str().compare(
                                          dmb->getAssocType()->getName().str());
    }
    return -1;
  }
  
  // - Other types.
  //
  // There should only ever be one of these in a set of constraints related to
  // a dependent type, so the ordering among other types does not matter.
  if (isa<GenericTypeParamType>(b) || isa<DependentMemberType>(b))
    return +1;
  return 0;
}

CanGenericSignature
GenericSignature::getCanonicalManglingSignature(Module &M) const {
  // Start from the elementwise-canonical signature.
  auto canonical = getCanonicalSignature();
  auto &Context = *canonical->CanonicalSignatureOrASTContext.get<ASTContext*>();
  
  // See if we cached the mangling signature.
  auto cached = Context.ManglingSignatures.find({canonical, &M});
  if (cached != Context.ManglingSignatures.end()) {
    return cached->second;
  }
  
  // Otherwise, we need to compute it.
  // Dump the generic signature into an ArchetypeBuilder that will figure out
  // the minimal set of requirements.
  ArchetypeBuilder builder(M, Context.Diags);
  
  builder.addGenericSignature(canonical, /*adoptArchetypes*/ false,
                              /*treatRequirementsAsExplicit*/ true);
  
  // Sort out the requirements.
  struct DependentConstraints {
    CanType baseClass;
    SmallVector<CanType, 2> protocols;
  };
  
  SmallVector<CanType, 2> depTypes;
  llvm::DenseMap<CanType, DependentConstraints> constraints;
  llvm::DenseMap<CanType, SmallVector<CanType, 2>> sameTypes;
  
  builder.enumerateRequirements([&](RequirementKind kind,
          ArchetypeBuilder::PotentialArchetype *archetype,
          llvm::PointerUnion<Type, ArchetypeBuilder::PotentialArchetype *> type,
          RequirementSource source) {
    CanType depTy
      = archetype->getDependentType(builder, false)->getCanonicalType();
    
    // Filter out redundant requirements.
    switch (source.getKind()) {
    case RequirementSource::Explicit:
      // The requirement was explicit and required, keep it.
      break;
      
    case RequirementSource::Protocol:
      // Keep witness markers.
      if (kind == RequirementKind::WitnessMarker)
        break;
      return;
    
    case RequirementSource::Redundant:
    case RequirementSource::Inferred:
      // The requirement was inferred or redundant, drop it.
      return;
      
    case RequirementSource::OuterScope:
      llvm_unreachable("shouldn't have an outer scope!");
    }
    
    switch (kind) {
    case RequirementKind::WitnessMarker: {
      // Introduce the dependent type into the constraint set, to ensure we
      // have a record for every dependent type.
      depTypes.push_back(depTy);
      return;
    }
      
    case RequirementKind::Conformance: {
      assert(std::find(depTypes.begin(), depTypes.end(),
                       depTy) != depTypes.end()
             && "didn't see witness marker first?");
      // Organize conformance constraints, sifting out the base class
      // requirement.
      auto &depConstraints = constraints[depTy];
      
      auto constraintType = type.get<Type>()->getCanonicalType();
      if (constraintType->isExistentialType()) {
        depConstraints.protocols.push_back(constraintType);
      } else {
        assert(depConstraints.baseClass.isNull()
               && "multiple base class constraints?!");
        depConstraints.baseClass = constraintType;
      }
        
      return;
    }
    
    case RequirementKind::SameType:
      // Collect the same-type constraints by their representative.
      CanType repTy;
      if (auto concreteTy = type.dyn_cast<Type>()) {
        // Maybe we were equated to a concrete type...
        repTy = concreteTy->getCanonicalType();
      } else {
        // ...or to a representative dependent type that was in turn equated
        // to a concrete type.
        auto representative
          = type.get<ArchetypeBuilder::PotentialArchetype *>();
        
        if (representative->isConcreteType())
          repTy = representative->getConcreteType()->getCanonicalType();
        else
          repTy = representative->getDependentType(builder, false)
            ->getCanonicalType();
      }
      
      sameTypes[repTy].push_back(depTy);
      return;
    }
  });
  
  // Order the dependent types canonically.
  llvm::array_pod_sort(depTypes.begin(), depTypes.end(), compareDependentTypes);
  
  // Build a new set of minimized requirements.
  // Emit the conformance constraints.
  SmallVector<Requirement, 4> minimalRequirements;
  for (auto depTy : depTypes) {
    minimalRequirements.push_back(Requirement(RequirementKind::WitnessMarker,
                                              depTy, Type()));
    
    auto foundConstraints = constraints.find(depTy);
    if (foundConstraints != constraints.end()) {
      const auto &depConstraints = foundConstraints->second;
      
      if (depConstraints.baseClass)
        minimalRequirements.push_back(Requirement(RequirementKind::Conformance,
                                                  depTy,
                                                  depConstraints.baseClass));
      
      for (auto protocol : depConstraints.protocols)
        minimalRequirements.push_back(Requirement(RequirementKind::Conformance,
                                                  depTy, protocol));
    }
  }
  
  // Collect the same type constraints.
  unsigned sameTypeBegin = minimalRequirements.size();
  
  for (auto &group : sameTypes) {
    // Sort the types in the set.
    auto types = std::move(group.second);
    types.push_back(group.first);
    llvm::array_pod_sort(types.begin(), types.end(), compareDependentTypes);

    // Form constraints with the greater type on the right (which will be the
    // concrete type, if one).
    auto rhsType = types.pop_back_val();
    for (auto lhsType : types)
      minimalRequirements.push_back(Requirement(RequirementKind::SameType,
                                                lhsType, rhsType));
  }
  
  // Sort the same-types by LHS, then by RHS.
  std::sort(minimalRequirements.begin() + sameTypeBegin, minimalRequirements.end(),
    [](const Requirement &a, const Requirement &b) -> bool {
      assert(a.getKind() == b.getKind()
             && a.getKind() == RequirementKind::SameType
             && "not same type constraints");
      CanType aLHS(a.getFirstType()), bLHS(b.getFirstType());
      if (int compareLHS = compareDependentTypes(&aLHS, &bLHS))
        return compareLHS < 0;
      CanType aRHS(a.getSecondType()), bRHS(b.getSecondType());
      return compareDependentTypes(&aRHS, &bRHS);
    });
  
  // Build the minimized signature.
  auto manglingSig = GenericSignature::get(canonical->getGenericParams(),
                                           minimalRequirements);
  
  CanGenericSignature canSig(manglingSig);
  
  // Cache the result.
  Context.ManglingSignatures.insert({{canonical, &M}, canSig});
  return canSig;
}

ASTContext &GenericSignature::getASTContext() const {
  return *getCanonicalSignature()->CanonicalSignatureOrASTContext
    .get<ASTContext *>();
}

TypeSubstitutionMap
GenericSignature::getSubstitutionMap(ArrayRef<Substitution> args) const {
  TypeSubstitutionMap subs;
  
  // An empty parameter list gives an empty map.
  if (getGenericParams().empty()) {
    assert(args.empty() && "substitutions but no generic params?!");
    return subs;
  }
  
  // Seed the type map with pre-existing substitutions.
  for (auto sub : args) {
    subs[sub.getArchetype()] = sub.getReplacement();
  }
  
  for (auto depTy : getAllDependentTypes()) {
    auto replacement = args.front().getReplacement();
    args = args.slice(1);
    
    if (auto subTy = depTy->getAs<SubstitutableType>()) {
      subs[subTy] = replacement;
    }
    else if (auto dTy = depTy->getAs<DependentMemberType>()) {
      subs[dTy] = replacement;
    }
  }
  
  assert(args.empty() && "did not use all substitutions?!");
  return subs;
}
