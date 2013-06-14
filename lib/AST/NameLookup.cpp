//===--- NameLookup.cpp - Swift Name Lookup Routines ----------------------===//
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
// This file implements interfaces for performing name lookup.
//
//===----------------------------------------------------------------------===//


#include "swift/AST/NameLookup.h"
#include "swift/AST/AST.h"
#include "swift/AST/ASTVisitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"

using namespace swift;

void swift::removeShadowedDecls(SmallVectorImpl<ValueDecl*> &decls,
                                bool isTypeLookup,
                                Module *curModule) {
  // Category declarations by their signatures.
  llvm::SmallDenseMap<CanType, llvm::TinyPtrVector<ValueDecl *>>
    declsBySignature;
  bool anyCollisions = false;
  for (auto decl : decls) {
    // Determine the signature of this declaration.
    // FIXME: the canonical type makes a poor signature, because we don't
    // canonicalize away default arguments and don't canonicalize polymorphic
    // types well.
    CanType signature;
    if (isTypeLookup && isa<TypeDecl>(decl))
      signature = cast<TypeDecl>(decl)->getDeclaredType()->getCanonicalType();
    else
      signature = decl->getType()->getCanonicalType();

    // If we've seen a declaration with this signature before, note it.
    auto &knownDecls = declsBySignature[signature];
    if (!knownDecls.empty())
      anyCollisions = true;

    knownDecls.push_back(decl);
  }

  // If there were no signature collisions, there is nothing to do.
  if (!anyCollisions)
    return;

  // Determine the set of declarations that are shadowed by other declarations.
  llvm::SmallPtrSet<ValueDecl *, 4> shadowed;
  for (auto &collidingDecls : declsBySignature) {
    // If only one declaration has this signature, it isn't shadowed by
    // anything.
    if (collidingDecls.second.size() == 1)
      continue;

    // Compare each declaration to every other declaration. This is
    // unavoidably O(n^2) in the number of declarations, but because they
    // all have the same signature, we expect n to remain small.
    for (unsigned firstIdx = 0, n = collidingDecls.second.size();
         firstIdx != n; ++firstIdx) {
      auto firstDecl = collidingDecls.second[firstIdx];
      auto firstDC = firstDecl->getDeclContext();
      auto firstModule = firstDecl->getModuleContext();
      for (unsigned secondIdx = firstIdx + 1; secondIdx != n; ++secondIdx) {
        // Determine whether one module takes precedence over another.
        auto secondDecl = collidingDecls.second[secondIdx];
        auto secondModule = secondDecl->getModuleContext();

        // If the first and second declarations are in the same module,
        // prefer one in the type itself vs. one in an extension.
        // FIXME: Should redeclaration checking prevent this from happening?
        if (firstModule == secondModule) {
          auto secondDC = secondDecl->getDeclContext();

          // If both declarations are in extensions, or both are in the
          // type definition itself, there's nothing we can do.
          if (isa<ExtensionDecl>(firstDC) == isa<ExtensionDecl>(secondDC))
            continue;

          // If the second declaration is in an extension, it is shadowed
          // by the first declaration. 
          if (isa<ExtensionDecl>(secondDC)) {
            shadowed.insert(secondDecl);
            continue;
          }

          // If the first declaration is in an extension, it is shadowed by
          // the second declaration. There is no point in continuing to compare
          // the first declaration to others.
          shadowed.insert(firstDecl);
          break;
        }

        // Prefer declarations in the current module over those in another
        // module.
        // FIXME: This is a hack. We should query a (lazily-built, cached)
        // module graph to determine shadowing.
        if ((firstModule == curModule) == (secondModule == curModule))
          continue;

        // If the first module is the current module, the second declaration
        // is shadowed by the first.
        if (firstModule == curModule) {
          shadowed.insert(secondDecl);
          continue;
        }

        // Otherwise, the first declaration is shadowed by the second. There is
        // no point in continuing to compare the first declaration to others.
        shadowed.insert(firstDecl);
        break;
      }
    }
  }

  // If none of the declarations were shadowed, we're done.
  if (shadowed.empty())
    return;

  // Remove shadowed declarations from the list of declarations.
  decls.erase(std::remove_if(decls.begin(), decls.end(),
                             [&](ValueDecl *vd) {
                               return shadowed.count(vd) > 0;
                             }),
              decls.end());
}

static void DoGlobalExtensionLookup(Type BaseType, Identifier Name,
                                    ArrayRef<ValueDecl*> BaseMembers,
                                    Module *CurModule,
                                    Module *BaseModule,
                                    bool IsTypeLookup,
                                    SmallVectorImpl<ValueDecl*> &Result) {
  auto nominal = BaseType->getAnyNominal();
  if (!nominal)
    return;

  // Add the members from the type itself to the list of results.
  for (auto member : BaseMembers) {
    if (member->getName() == Name)
      Result.push_back(member);
  }

  // Look in each extension for declarations with this name.
  for (auto extension : nominal->getExtensions()) {
    for (auto member : extension->getMembers()) {
      auto vd = dyn_cast<ValueDecl>(member);
      if (!vd)
        continue;

      if (vd->getName() != Name)
        continue;

      Result.push_back(vd);
    }
  }

  // Handle shadowing.
  removeShadowedDecls(Result, IsTypeLookup, CurModule);
}

MemberLookup::MemberLookup(Type BaseTy, Identifier Name, Module &M,
                           bool TypeLookup) {
  MemberName = Name;
  IsTypeLookup = TypeLookup;
  VisitedSet Visited;
  doIt(BaseTy, M, /*OnlyInstanceMembers=*/!TypeLookup, Visited);
}

/// doIt - Lookup a member 'Name' in 'BaseTy' within the context
/// of a given module 'M'.  This operation corresponds to a standard "dot" 
/// lookup operation like "a.b" where 'this' is the type of 'a'.  This
/// operation is only valid after name binding.
///
/// \param OnlyInstanceMembers Only instance members should be found by
/// name lookup.
void MemberLookup::doIt(Type BaseTy, Module &M, bool OnlyInstanceMembers,
                        VisitedSet &Visited) {
  typedef MemberLookupResult Result;
  
  // Just look through l-valueness.  It doesn't affect name lookup.
  BaseTy = BaseTy->getRValueType();

  // Type check metatype references, as in "some_type.some_member".  These are
  // special and can't have extensions.
  if (MetaTypeType *MTT = BaseTy->getAs<MetaTypeType>()) {
    // The metatype represents an arbitrary named type: dig through to the
    // declared type to see what we're dealing with.
    Type Ty = MTT->getInstanceType();

    // Just perform normal dot lookup on the type with the specified
    // member name to see if we find extensions or anything else.  For example,
    // type SomeTy.SomeMember can look up static functions, and can even look
    // up non-static functions as well (thus getting the address of the member).
    doIt(Ty, M, /*OnlyInstanceMembers=*/false, Visited);
    return;
  }
  
  // Lookup module references, as on some_module.some_member.  These are
  // special and can't have extensions.
  if (ModuleType *MT = BaseTy->getAs<ModuleType>()) {
    SmallVector<ValueDecl*, 8> Decls;
    MT->getModule()->lookupValue(Module::AccessPathTy(), MemberName,
                                 NLKind::QualifiedLookup, Decls);
    for (ValueDecl *VD : Decls) {
      Results.push_back(Result::getMetatypeMember(VD));
    }
    return;
  }

  // If the base is a protocol, see if this is a reference to a declared
  // protocol member.
  if (ProtocolType *PT = BaseTy->getAs<ProtocolType>()) {
    if (!Visited.insert(PT->getDecl()))
      return;
      
    for (auto Inherited : PT->getDecl()->getInherited())
      doIt(Inherited.getType(), M, OnlyInstanceMembers, Visited);
    
    for (auto Member : PT->getDecl()->getMembers()) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(Member)) {
        if (VD->getName() != MemberName) continue;
        if (isa<VarDecl>(VD) || isa<SubscriptDecl>(VD) || isa<FuncDecl>(VD)) {
          if (OnlyInstanceMembers && !VD->isInstanceMember())
            continue;

          Results.push_back(Result::getExistentialMember(VD));
        } else {
          assert(isa<TypeDecl>(VD) && "Unhandled protocol member");
          Results.push_back(Result::getMetatypeMember(VD));
        }
      }
    }
    return;
  }
  
  // If the base is a protocol composition, see if this is a reference to a
  // declared protocol member in any of the protocols.
  if (auto PC = BaseTy->getAs<ProtocolCompositionType>()) {
    for (auto Proto : PC->getProtocols())
      doIt(Proto, M, OnlyInstanceMembers, Visited);
    return;
  }

  // Check to see if any of an archetype's requirements have the member.
  if (ArchetypeType *Archetype = BaseTy->getAs<ArchetypeType>()) {
    for (auto Proto : Archetype->getConformsTo())
      doIt(Proto->getDeclaredType(), M, OnlyInstanceMembers, Visited);

    if (auto superclass = Archetype->getSuperclass())
      doIt(superclass, M, OnlyInstanceMembers, Visited);

    // Change existential and metatype members to archetype members, since
    // we're in an archetype.
    for (auto &Result : Results) {
      switch (Result.Kind) {
      case MemberLookupResult::ExistentialMember:
        Result.Kind = MemberLookupResult::ArchetypeMember;
        break;

      case MemberLookupResult::MetatypeMember:
        Result.Kind = MemberLookupResult::MetaArchetypeMember;
        break;

      case MemberLookupResult::MemberProperty:
      case MemberLookupResult::MemberFunction:
      case MemberLookupResult::GenericParameter:
        break;
          
      case MemberLookupResult::MetaArchetypeMember:
      case MemberLookupResult::ArchetypeMember:
        llvm_unreachable("wrong member lookup result in archetype");
        break;
      }
    }
    return;
  }

  do {
    // Look in for members of a nominal type.
    SmallVector<ValueDecl*, 8> ExtensionMethods;
    lookupMembers(BaseTy, M, ExtensionMethods);

    for (ValueDecl *VD : ExtensionMethods) {
      if (TypeDecl *TD = dyn_cast<TypeDecl>(VD)) {
        auto TAD = dyn_cast<TypeAliasDecl>(TD);
        if (TAD && TAD->isGenericParameter())
          Results.push_back(Result::getGenericParameter(TAD));
        else
          Results.push_back(Result::getMetatypeMember(TD));
        continue;
      }

      if (OnlyInstanceMembers && !VD->isInstanceMember())
        continue;

      if (FuncDecl *FD = dyn_cast<FuncDecl>(VD)) {
        if (FD->isStatic())
          Results.push_back(Result::getMetatypeMember(FD));
        else
          Results.push_back(Result::getMemberFunction(FD));
        continue;
      }
      if (OneOfElementDecl *OOED = dyn_cast<OneOfElementDecl>(VD)) {
        Results.push_back(Result::getMetatypeMember(OOED));
        continue;
      }
      assert((isa<VarDecl>(VD) || isa<SubscriptDecl>(VD)) &&
             "Unexpected extension member");
      Results.push_back(Result::getMemberProperty(VD));
    }

    // If we have a class type, look into its base class.
    ClassDecl *CurClass = nullptr;
    if (auto CT = BaseTy->getAs<ClassType>())
      CurClass = CT->getDecl();
    else if (auto BGT = BaseTy->getAs<BoundGenericType>())
      CurClass = dyn_cast<ClassDecl>(BGT->getDecl());
    else if (UnboundGenericType *UGT = BaseTy->getAs<UnboundGenericType>())
      CurClass = dyn_cast<ClassDecl>(UGT->getDecl());

    if (CurClass && CurClass->hasBaseClass()) {
      BaseTy = CurClass->getBaseClass();
    } else {
      break;
    }
  } while (1);

  // Find any overridden methods.
  llvm::SmallPtrSet<ValueDecl*, 8> Overridden;
  for (const auto &Result : Results) {
    if (auto FD = dyn_cast<FuncDecl>(Result.D)) {
      if (FD->getOverriddenDecl())
        Overridden.insert(FD->getOverriddenDecl());
    } else if (auto VarD = dyn_cast<VarDecl>(Result.D)) {
      if (VarD->getOverriddenDecl())
        Overridden.insert(VarD->getOverriddenDecl());
    } else if (auto SD = dyn_cast<SubscriptDecl>(Result.D)) {
      if (SD->getOverriddenDecl())
        Overridden.insert(SD->getOverriddenDecl());
    }
  }

  // If any methods were overridden, remove them from the results.
  if (!Overridden.empty()) {
    Results.erase(std::remove_if(Results.begin(), Results.end(),
                                 [&](MemberLookupResult &Res) -> bool {
                                   return Overridden.count(Res.D);
                                 }),
                  Results.end());
  }
}

void MemberLookup::lookupMembers(Type BaseType, Module &M,
                                 SmallVectorImpl<ValueDecl*> &Result) {
  NominalTypeDecl *D;
  ArrayRef<ValueDecl*> BaseMembers;
  SmallVector<ValueDecl*, 2> BaseMembersStorage;
  if (BoundGenericType *BGT = BaseType->getAs<BoundGenericType>()) {
    BaseType = BGT->getDecl()->getDeclaredType();
    D = BGT->getDecl();
  } else if (UnboundGenericType *UGT = BaseType->getAs<UnboundGenericType>()) {
    D = UGT->getDecl();
  } else if (NominalType *NT = BaseType->getAs<NominalType>()) {
    D = NT->getDecl();
  } else {
    return;
  }

  for (Decl* Member : D->getMembers()) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(Member)) {
      BaseMembersStorage.push_back(VD);
    }
  }
  if (D->getGenericParams())
    for (auto param : *D->getGenericParams())
      BaseMembersStorage.push_back(param.getDecl());
  BaseMembers = BaseMembersStorage;

  DeclContext *DC = D->getDeclContext();
  while (!DC->isModuleContext())
    DC = DC->getParent();

  DoGlobalExtensionLookup(BaseType, MemberName, BaseMembers, &M,
                          cast<Module>(DC), IsTypeLookup, Result);
}

ConstructorLookup::ConstructorLookup(Type BaseType, Module &M) {
  NominalTypeDecl *D;
  if (NominalType *NT = BaseType->getAs<NominalType>())
    D = NT->getDecl();
  else if (BoundGenericType *BGT = BaseType->getAs<BoundGenericType>())
    D = BGT->getDecl();
  else
    return;

  SmallVector<ValueDecl*, 16> BaseMembers;
  if (StructDecl *SD = dyn_cast<StructDecl>(D)) {
    for (Decl* Member : SD->getMembers()) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(Member))
        BaseMembers.push_back(VD);
    }
  } else if (OneOfDecl *OOD = dyn_cast<OneOfDecl>(D)) {
    for (Decl* Member : OOD->getMembers()) {
      // FIXME: We shouldn't be injecting OneOfElementDecls into the results
      // like this.
      if (OneOfElementDecl *OOED = dyn_cast<OneOfElementDecl>(Member))
        Results.push_back(OOED);
      else if (ValueDecl *VD = dyn_cast<ValueDecl>(Member))
        BaseMembers.push_back(VD);
    }
  } else if (ClassDecl *CD = dyn_cast<ClassDecl>(D)) {
    for (Decl* Member : CD->getMembers()) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(Member))
        BaseMembers.push_back(VD);
    }
  } else {
    return;
  }

  Identifier Constructor = M.Ctx.getIdentifier("constructor");
  DeclContext *DC = D->getDeclContext();
  if (!DC->isModuleContext()) {
    for (ValueDecl *VD : BaseMembers) {
      if (VD->getName() == Constructor)
        Results.push_back(VD);
    }
    return;
  }

  DoGlobalExtensionLookup(BaseType, Constructor, BaseMembers, &M,
                          cast<Module>(DC), /*IsTypeLookup*/false, Results);
}

struct FindLocalVal : public StmtVisitor<FindLocalVal> {
  SourceLoc Loc;
  Identifier Name;
  ValueDecl *MatchingValue;

  FindLocalVal(SourceLoc Loc, Identifier Name)
    : Loc(Loc), Name(Name), MatchingValue(nullptr) {}

  bool IntersectsRange(SourceRange R) {
    return R.Start.Value.getPointer() <= Loc.Value.getPointer() &&
           R.End.Value.getPointer() >= Loc.Value.getPointer();
  }

  void checkValueDecl(ValueDecl *D) {
    if (D->getName() == Name) {
      assert(!MatchingValue);
      MatchingValue = D;
    }
  }

  void checkPattern(Pattern *Pat) {
    switch (Pat->getKind()) {
    case PatternKind::Tuple:
      for (auto &field : cast<TuplePattern>(Pat)->getFields())
        checkPattern(field.getPattern());
      return;
    case PatternKind::Paren:
      return checkPattern(cast<ParenPattern>(Pat)->getSubPattern());
    case PatternKind::Typed:
      return checkPattern(cast<TypedPattern>(Pat)->getSubPattern());
    case PatternKind::Named:
      return checkValueDecl(cast<NamedPattern>(Pat)->getDecl());
    // Handle non-vars.
    case PatternKind::Any:
      return;
    }
  }

  void checkGenericParams(GenericParamList *Params) {
    if (!Params)
      return;

    for (auto P : *Params)
      checkValueDecl(P.getDecl());
  }

  void checkTranslationUnit(TranslationUnit *TU) {
    for (Decl *D : TU->Decls) {
      if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D))
        visit(TLCD->getBody());
    }
  }

  void visitBreakStmt(BreakStmt *) {}
  void visitContinueStmt(ContinueStmt *) {}
  void visitFallthroughStmt(FallthroughStmt *) {}
  void visitReturnStmt(ReturnStmt *) {}
  void visitIfStmt(IfStmt * S) {
    visit(S->getThenStmt());
    if (S->getElseStmt())
      visit(S->getElseStmt());
  }
  void visitWhileStmt(WhileStmt *S) {
    visit(S->getBody());
  }
  void visitDoWhileStmt(DoWhileStmt *S) {
    visit(S->getBody());
  }

  void visitForStmt(ForStmt *S) {
    if (!IntersectsRange(S->getSourceRange()))
      return;
    visit(S->getBody());
    if (MatchingValue)
      return;
    for (Decl *D : S->getInitializerVarDecls()) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
        checkValueDecl(VD);
    }
  }
  void visitForEachStmt(ForEachStmt *S) {
    if (!IntersectsRange(S->getSourceRange()))
      return;
    visit(S->getBody());
    if (MatchingValue)
      return;
    checkPattern(S->getPattern());
  }
  void visitBraceStmt(BraceStmt *S) {
    if (!IntersectsRange(S->getSourceRange()))
      return;
    for (auto elem : S->getElements()) {
      if (Stmt *S = elem.dyn_cast<Stmt*>())
        visit(S);
    }
    if (MatchingValue)
      return;
    for (auto elem : S->getElements()) {
      if (Decl *D = elem.dyn_cast<Decl*>()) {
        if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
          checkValueDecl(VD);
      }
    }
  }
  void visitSwitchStmt(SwitchStmt *S) {
    if (!IntersectsRange(S->getSourceRange()))
      return;
    for (CaseStmt *C : S->getCases()) {
      visit(C);
    }
  }
  
  void visitCaseStmt(CaseStmt *S) {
    if (!IntersectsRange(S->getSourceRange()))
      return;
    
    // TODO: Check patterns in pattern-matching case.
    visit(S->getBody());
  }
};

UnqualifiedLookup::UnqualifiedLookup(Identifier Name, DeclContext *DC,
                                     SourceLoc Loc, bool IsTypeLookup) {
  typedef UnqualifiedLookupResult Result;

  DeclContext *ModuleDC = DC;
  while (!ModuleDC->isModuleContext())
    ModuleDC = ModuleDC->getParent();

  Module &M = *cast<Module>(ModuleDC);

  // Never perform local lookup for operators.
  if (Name.isOperator())
    DC = ModuleDC;

  // If we are inside of a method, check to see if there are any ivars in scope,
  // and if so, whether this is a reference to one of them.
  while (!DC->isModuleContext()) {
    ValueDecl *BaseDecl = 0;
    ValueDecl *MetaBaseDecl = 0;
    GenericParamList *GenericParams = nullptr;
    Type ExtendedType;
    if (FuncExpr *FE = dyn_cast<FuncExpr>(DC)) {
      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      if (Loc.isValid()) {
        FindLocalVal localVal(Loc, Name);
        localVal.visit(FE->getBody());
        if (!localVal.MatchingValue) {
          for (Pattern *P : FE->getBodyParamPatterns())
            localVal.checkPattern(P);
        }
        if (localVal.MatchingValue) {
          Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
          return;
        }
      }

      FuncDecl *FD = FE->getDecl();
      if (FD && FD->getExtensionType()) {
        ExtendedType = FD->getExtensionType();
        BaseDecl = FD->getImplicitThisDecl();
        if (NominalType *NT = ExtendedType->getAs<NominalType>())
          MetaBaseDecl = NT->getDecl();
        else if (auto UGT = ExtendedType->getAs<UnboundGenericType>())
          MetaBaseDecl = UGT->getDecl();
        DC = DC->getParent();

        if (FD->isStatic())
          ExtendedType = MetaTypeType::get(ExtendedType, M.getASTContext());
      }

      // Look in the generic parameters after checking our local declaration.
      if (FD)
        GenericParams = FD->getGenericParams();
    } else if (PipeClosureExpr *CE = dyn_cast<PipeClosureExpr>(DC)) {
      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      if (Loc.isValid()) {
        FindLocalVal localVal(Loc, Name);
        localVal.visit(CE->getBody());
        if (!localVal.MatchingValue) {
          localVal.checkPattern(CE->getParams());
        }
        if (localVal.MatchingValue) {
          Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
          return;
        }
      }
    } else if (ExtensionDecl *ED = dyn_cast<ExtensionDecl>(DC)) {
      ExtendedType = ED->getExtendedType();
      if (NominalType *NT = ExtendedType->getAs<NominalType>())
        BaseDecl = NT->getDecl();
      else if (auto UGT = ExtendedType->getAs<UnboundGenericType>())
        BaseDecl = UGT->getDecl();
      MetaBaseDecl = BaseDecl;
    } else if (NominalTypeDecl *ND = dyn_cast<NominalTypeDecl>(DC)) {
      ExtendedType = ND->getDeclaredType();
      BaseDecl = ND;
      MetaBaseDecl = BaseDecl;
    } else if (ConstructorDecl *CD = dyn_cast<ConstructorDecl>(DC)) {
      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      if (Loc.isValid()) {
        FindLocalVal localVal(Loc, Name);
        localVal.visit(CD->getBody());
        if (!localVal.MatchingValue)
          localVal.checkPattern(CD->getArguments());
        if (localVal.MatchingValue) {
          Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
          return;
        }
      }

      BaseDecl = CD->getImplicitThisDecl();
      ExtendedType = CD->getDeclContext()->getDeclaredTypeOfContext();
      if (NominalType *NT = ExtendedType->getAs<NominalType>())
        MetaBaseDecl = NT->getDecl();
      else if (auto UGT = ExtendedType->getAs<UnboundGenericType>())
        MetaBaseDecl = UGT->getDecl();
      DC = DC->getParent();
    } else if (DestructorDecl *DD = dyn_cast<DestructorDecl>(DC)) {
      // Look for local variables; normally, the parser resolves these
      // for us, but it can't do the right thing inside local types.
      if (Loc.isValid()) {
        FindLocalVal localVal(Loc, Name);
        localVal.visit(CD->getBody());
        if (!localVal.MatchingValue)
          localVal.checkPattern(CD->getArguments());
        if (localVal.MatchingValue) {
          Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
          return;
        }
      }

      BaseDecl = DD->getImplicitThisDecl();
      ExtendedType = DD->getDeclContext()->getDeclaredTypeOfContext();
      if (NominalType *NT = ExtendedType->getAs<NominalType>())
        MetaBaseDecl = NT->getDecl();
      else if (auto UGT = ExtendedType->getAs<UnboundGenericType>())
        MetaBaseDecl = UGT->getDecl();
      DC = DC->getParent();
    }

    if (BaseDecl) {
      MemberLookup Lookup(ExtendedType, Name, M, IsTypeLookup);
      
      for (auto Result : Lookup.Results) {
        switch (Result.Kind) {
        case MemberLookupResult::MemberProperty:
          Results.push_back(Result::getMemberProperty(BaseDecl, Result.D));
          break;
        case MemberLookupResult::MemberFunction:
          Results.push_back(Result::getMemberFunction(BaseDecl, Result.D));
          break;
        case MemberLookupResult::MetatypeMember:
          // For results that can only be accessed via the metatype (e.g.,
          // type aliases), we need to use the metatype declaration as the
          // base.
          Results.push_back(Result::getMetatypeMember(isa<FuncDecl>(Result.D)?
                                                        BaseDecl : MetaBaseDecl,
                                                      Result.D));
          break;
        case MemberLookupResult::ExistentialMember:
          Results.push_back(Result::getExistentialMember(BaseDecl, Result.D));
          break;
        case MemberLookupResult::ArchetypeMember:
          Results.push_back(Result::getArchetypeMember(BaseDecl, Result.D));
          break;
        case MemberLookupResult::MetaArchetypeMember:
          // For results that can only be accessed via the metatype (e.g.,
          // type aliases), we need to use the metatype declaration as the
          // base.
          Results.push_back(Result::getMetaArchetypeMember(
                              isa<FuncDecl>(Result.D)? BaseDecl : MetaBaseDecl,
                              Result.D));
          break;
        case MemberLookupResult::GenericParameter:
          // All generic parameters are 'local'.
          Results.push_back(Result::getLocalDecl(Result.D));
          break;
        }
      }
      if (Lookup.isSuccess())
        return;
    }

    // Check the generic parameters for something with the given name.
    if (GenericParams) {
      FindLocalVal localVal(Loc, Name);
      localVal.checkGenericParams(GenericParams);

      if (localVal.MatchingValue) {
        Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
        return;
      }
    }

    DC = DC->getParent();
  }

  if (Loc.isValid()) {
    if (TranslationUnit *TU = dyn_cast<TranslationUnit>(&M)) {
      // Look for local variables in top-level code; normally, the parser
      // resolves these for us, but it can't do the right thing for
      // local types.
      FindLocalVal localVal(Loc, Name);
      localVal.checkTranslationUnit(TU);
      if (localVal.MatchingValue) {
        Results.push_back(Result::getLocalDecl(localVal.MatchingValue));
        return;
      }
    }
  }

  // Track whether we've already searched the Clang modules.
  // FIXME: This is a weird hack. We either need to filter within the
  // Clang module importer, or we need to change how this works.
  bool searchedClangModule = false;
  
  // Do a local lookup within the current module.
  llvm::SmallVector<ValueDecl*, 4> CurModuleResults;
  M.lookupValue(Module::AccessPathTy(), Name, NLKind::UnqualifiedLookup,
                CurModuleResults);
  searchedClangModule = isa<ClangModule>(&M);
  for (ValueDecl *VD : CurModuleResults)
    if (!IsTypeLookup || isa<TypeDecl>(VD))
      Results.push_back(Result::getModuleMember(VD));

  // The builtin module has no imports.
  if (isa<BuiltinModule>(M)) return;
  
  TranslationUnit &TU = cast<TranslationUnit>(M);

  llvm::SmallPtrSet<CanType, 8> CurModuleTypes;
  for (ValueDecl *VD : CurModuleResults) {
    // If we find a type in the current module, don't look into any
    // imported modules.
    if (isa<TypeDecl>(VD))
      return;
    if (!IsTypeLookup)
      CurModuleTypes.insert(VD->getType()->getCanonicalType());
  }

  // Scrape through all of the imports looking for additional results.
  // FIXME: Implement DAG-based shadowing rules.
  llvm::SmallPtrSet<Module *, 16> Visited;
  for (auto &ImpEntry : TU.getImportedModules()) {
    if (!Visited.insert(ImpEntry.second))
      continue;

    // FIXME: Only searching Clang modules once.
    if (isa<ClangModule>(ImpEntry.second)) {
      if (searchedClangModule)
        continue;

      searchedClangModule = true;
    }

    SmallVector<ValueDecl*, 8> ImportedModuleResults;
    ImpEntry.second->lookupValue(ImpEntry.first, Name, NLKind::UnqualifiedLookup,
                                 ImportedModuleResults);
    for (ValueDecl *VD : ImportedModuleResults) {
      if ((!IsTypeLookup || isa<TypeDecl>(VD)) &&
          !CurModuleTypes.count(VD->getType()->getCanonicalType())) {
        Results.push_back(Result::getModuleMember(VD));
      }
    }
  }

  // If we've found something, we're done.
  if (!Results.empty())
    return;

  // Look for a module with the given name.
  if (Name == M.Name) {
    Results.push_back(Result::getModuleName(&M));
  } else {
    for (const auto &ImpEntry : TU.getImportedModules())
      if (ImpEntry.second->Name == Name) {
        Results.push_back(Result::getModuleName(ImpEntry.second));
        break;
      }
  }
}

Optional<UnqualifiedLookup>
UnqualifiedLookup::forModuleAndName(ASTContext &C,
                                    StringRef Mod, StringRef Name) {
  auto foundModule = C.LoadedModules.find(Mod);
  if (foundModule == C.LoadedModules.end())
    return Nothing;

  Module *m = foundModule->second;
  return UnqualifiedLookup(C.getIdentifier(Name), m);
}

TypeDecl* UnqualifiedLookup::getSingleTypeResult() {
  if (Results.size() != 1 || !Results.back().hasValueDecl() ||
      !isa<TypeDecl>(Results.back().getValueDecl()))
    return nullptr;
  return cast<TypeDecl>(Results.back().getValueDecl());
}
