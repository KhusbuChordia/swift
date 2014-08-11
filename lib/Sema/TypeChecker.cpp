//===--- TypeChecker.cpp - Type Checking ----------------------------------===//
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
// This file implements the swift::performTypeChecking entry point for
// semantic analysis.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/STLExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

TypeChecker::TypeChecker(ASTContext &Ctx, DiagnosticEngine &Diags)
  : Context(Ctx), Diags(Diags)
{
  auto clangImporter =
    static_cast<ClangImporter *>(Context.getClangModuleLoader());
  clangImporter->setTypeResolver(*this);

}

TypeChecker::~TypeChecker() {
  auto clangImporter =
    static_cast<ClangImporter *>(Context.getClangModuleLoader());
  clangImporter->clearTypeResolver();
}

void TypeChecker::handleExternalDecl(Decl *decl) {
  if (auto SD = dyn_cast<StructDecl>(decl)) {\
    SmallVector<Decl*, 2> NewInits;
    addImplicitConstructors(SD, NewInits);
    addImplicitStructConformances(SD);
  }
  if (auto CD = dyn_cast<ClassDecl>(decl)) {
    addImplicitDestructor(CD);
  }
  if (auto ED = dyn_cast<EnumDecl>(decl)) {
    addImplicitEnumConformances(ED);
  }
}

ProtocolDecl *TypeChecker::getProtocol(SourceLoc loc, KnownProtocolKind kind) {
  auto protocol = Context.getProtocol(kind);
  if (!protocol && loc.isValid()) {
    diagnose(loc, diag::missing_protocol,
             Context.getIdentifier(getProtocolName(kind)));
  }

  if (protocol && !protocol->hasType()) {
    validateDecl(protocol);
    if (protocol->isInvalid())
      return nullptr;
  }

  return protocol;
}

ProtocolDecl *TypeChecker::getLiteralProtocol(Expr *expr) {
  if (isa<ArrayExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::ArrayLiteralConvertible);

  if (isa<DictionaryExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::DictionaryLiteralConvertible);

  if (!isa<LiteralExpr>(expr))
    return nullptr;
  
  if (isa<NilLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::NilLiteralConvertible);
  
  if (isa<IntegerLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::IntegerLiteralConvertible);

  if (isa<FloatLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::FloatLiteralConvertible);

  if (isa<BooleanLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::BooleanLiteralConvertible);

  if (isa<CharacterLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::CharacterLiteralConvertible);

  if (const auto *SLE = dyn_cast<StringLiteralExpr>(expr)) {
    if (SLE->isSingleExtendedGraphemeCluster())
      return getProtocol(
          expr->getLoc(),
          KnownProtocolKind::ExtendedGraphemeClusterLiteralConvertible);
    else
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringLiteralConvertible);
  }

  if (isa<InterpolatedStringLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::StringInterpolationConvertible);

  if (auto E = dyn_cast<MagicIdentifierLiteralExpr>(expr)) {
    switch (E->getKind()) {
    case MagicIdentifierLiteralExpr::File:
    case MagicIdentifierLiteralExpr::Function:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringLiteralConvertible);

    case MagicIdentifierLiteralExpr::Line:
    case MagicIdentifierLiteralExpr::Column:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::IntegerLiteralConvertible);
    }
  }
  
  return nullptr;
}

Module *TypeChecker::getStdlibModule(const DeclContext *dc) {
  if (StdlibModule)
    return StdlibModule;

  if (!StdlibModule)
    StdlibModule = Context.getStdlibModule();
  if (!StdlibModule)
    StdlibModule = dc->getParentModule();

  assert(StdlibModule && "no main module found");
  Context.recordKnownProtocols(StdlibModule);
  return StdlibModule;
}

Type TypeChecker::lookupBoolType(const DeclContext *dc) {
  return boolType.cache([&]{
    UnqualifiedLookup boolLookup(Context.getIdentifier("Bool"),
                                 getStdlibModule(dc), nullptr,
                                 SourceLoc(),
                                 /*IsTypeLookup=*/true);
    if (!boolLookup.isSuccess()) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    TypeDecl *tyDecl = boolLookup.getSingleTypeResult();
    
    if (!tyDecl) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    
    return tyDecl->getDeclaredType();
  });
}

static void bindExtensionDecl(ExtensionDecl *ED, TypeChecker &TC) {
  if (ED->getExtendedType())
    return;

  auto dc = ED->getDeclContext();

  // Synthesize a type representation for the extended type.
  SmallVector<ComponentIdentTypeRepr *, 2> components;
  for (auto &ref : ED->getRefComponents()) {
    // A reference to ".Type" is an attempt to extend the metatype.
    if (ref.Name == TC.Context.Id_Type && !components.empty()) {
      TC.diagnose(ref.NameLoc, diag::extension_metatype);
      ED->setInvalid();
      ED->setExtendedType(ErrorType::get(TC.Context));
      return;
    }

    components.push_back(
      new (TC.Context) SimpleIdentTypeRepr(ref.NameLoc, ref.Name));
  }

  // Validate the representation.
  TypeLoc typeLoc(IdentTypeRepr::create(TC.Context, components));
  if (TC.validateType(typeLoc, dc, TR_AllowUnboundGenerics)) {
    ED->setInvalid();
    ED->setExtendedType(ErrorType::get(TC.Context));
    return;
  }

  // Check the generic parameter lists for each of the components.
  GenericParamList *outerGenericParams = nullptr;
  for (unsigned i = 0, n = components.size(); i != n; ++i) {
    // Find the type declaration to which the identifier type actually referred.
    auto ident = components[i];
    NominalTypeDecl *typeDecl = nullptr;
    if (auto type = ident->getBoundType()) {
      if (auto unbound = dyn_cast<UnboundGenericType>(type.getPointer()))
        typeDecl = unbound->getDecl();
      else if (auto nominal = dyn_cast<NominalType>(type.getPointer()))
        typeDecl = nominal->getDecl();
    } else if (auto decl = ident->getBoundDecl()) {
      typeDecl = dyn_cast<NominalTypeDecl>(decl);
    }

    // FIXME: There are more restrictions on what we can refer to, e.g.,
    // we can't look through a typealias to a bound generic type of any form.

    // We aren't referring to a type declaration, so make sure we don't have
    // generic arguments.
    auto &ref = ED->getRefComponents()[i];
    if (!typeDecl) {
      // FIXME: This diagnostic is awful. It should point at what we did find,
      // e.g., a type, module, etc.
      if (ref.GenericParams) {
        TC.diagnose(ref.NameLoc, diag::extension_generic_params_for_non_generic,
                    ref.Name);
        ref.GenericParams = nullptr;
      }

      continue;
    }

    // The extended type is generic but the extension does not have generic
    // parameters.
    // FIXME: This will eventually become a Fix-It.
    if (typeDecl->getGenericParams() && !ref.GenericParams) {
      continue;
    }

    // The extended type is non-generic but the extension has generic
    // parameters. Complain and drop them.
    if (!typeDecl->getGenericParams() && ref.GenericParams) {
      TC.diagnose(ref.NameLoc,
                  diag::extension_generic_params_for_non_generic_type,
                  typeDecl->getDeclaredType())
        .highlight(ref.GenericParams->getSourceRange());
      TC.diagnose(typeDecl, diag::extended_type_here,
                  typeDecl->getDeclaredType());
      ref.GenericParams = nullptr;
      continue;
    }

    // If neither has generic parameters, we're done.
    if (!ref.GenericParams)
      continue;

    // Both have generic parameters: check that we have the right number of
    // parameters. Semantic checks will wait for extension validation.
    if (ref.GenericParams->size() != typeDecl->getGenericParams()->size()) {
      unsigned numHave = ref.GenericParams->size();
      unsigned numExpected = typeDecl->getGenericParams()->size();
      TC.diagnose(ref.NameLoc,
                  diag::extension_generic_wrong_number_of_parameters,
                  typeDecl->getDeclaredType(), numHave > numExpected,
                  numHave, numExpected)
        .highlight(ref.GenericParams->getSourceRange());
      ED->setInvalid();
      ED->setExtendedType(ErrorType::get(TC.Context));
      return;
    }

    // Chain the generic parameters together.
    ref.GenericParams->setOuterParameters(outerGenericParams);
    outerGenericParams = ref.GenericParams;
  }

  // Check whether we extended something that is not a nominal type.
  Type extendedTy = typeLoc.getType();
  if (!extendedTy->is<NominalType>() && !extendedTy->is<UnboundGenericType>()) {
    TC.diagnose(ED, diag::non_nominal_extension, false, extendedTy);
    ED->setInvalid();
    ED->setExtendedType(ErrorType::get(TC.Context));
    return;
  }

  ED->setExtendedType(extendedTy);
  if (auto nominal = extendedTy->getAnyNominal())
    nominal->addExtension(ED);
}

/// Returns true if the given decl or extension conforms to a protocol whose
/// name matches a compiler-known protocol. This is a syntactic check; no type
/// resolution is performed.
template <typename DeclTy>
static bool mayConformToKnownProtocol(const DeclTy *D) {
  for (TypeLoc inherited : D->getInherited()) {
    auto identRepr = dyn_cast_or_null<IdentTypeRepr>(inherited.getTypeRepr());
    if (!identRepr)
      continue;

    auto lastSimpleID =
        dyn_cast<SimpleIdentTypeRepr>(identRepr->getComponentRange().back());
    if (!lastSimpleID)
      continue;

    bool matchesKnownProtocol =
      llvm::StringSwitch<bool>(lastSimpleID->getIdentifier().str())
#define PROTOCOL(Name) \
        .Case(#Name, true)
#include "swift/AST/KnownProtocols.def"
        .Default(false);
    if (matchesKnownProtocol)
      return true;
  }

  return false;
}

static void typeCheckFunctionsAndExternalDecls(TypeChecker &TC) {
  unsigned currentFunctionIdx = 0;
  unsigned currentExternalDef = TC.Context.LastCheckedExternalDefinition;
  do {
    
    for (unsigned n = TC.Context.ExternalDefinitions.size();
         currentExternalDef != n;
         ++currentExternalDef) {
      auto decl = TC.Context.ExternalDefinitions[currentExternalDef];
      
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl)) {
        PrettyStackTraceDecl StackEntry("type-checking", AFD);
        TC.typeCheckAbstractFunctionBody(AFD);
        continue;
      }
      if (isa<NominalTypeDecl>(decl)) {
        TC.handleExternalDecl(decl);
        continue;
      }
      llvm_unreachable("Unhandled external definition kind");
    }
    
    // Type check the body of each of the function in turn.  Note that outside
    // functions must be visited before nested functions for type-checking to
    // work correctly.
    unsigned previousFunctionIdx = currentFunctionIdx;
    for (unsigned n = TC.definedFunctions.size(); currentFunctionIdx != n;
         ++currentFunctionIdx) {
      auto *AFD = TC.definedFunctions[currentFunctionIdx];
      PrettyStackTraceDecl StackEntry("type-checking", AFD);
      TC.typeCheckAbstractFunctionBody(AFD);
    }

    // Compute captures for functions we visited, in the opposite order of type
    // checking. i.e., the nested DefinedFunctions will be visited before the
    // outer DefinedFunctions.
    for (unsigned i = currentFunctionIdx; i > previousFunctionIdx; --i) {
      if (auto *FD = dyn_cast<AbstractFunctionDecl>(TC.definedFunctions[i-1]))
        TC.computeCaptures(FD);
    }

    // Type-check any referenced nominal types.
    while (!TC.ValidatedTypes.empty()) {
      auto nominal = TC.ValidatedTypes.back();
      TC.ValidatedTypes.pop_back();
      TC.typeCheckDecl(nominal, /*isFirstPass=*/true);
    }

    TC.definedFunctions.insert(TC.definedFunctions.end(),
                               TC.implicitlyDefinedFunctions.begin(),
                               TC.implicitlyDefinedFunctions.end());
    TC.implicitlyDefinedFunctions.clear();

  } while (currentFunctionIdx < TC.definedFunctions.size() ||
           currentExternalDef < TC.Context.ExternalDefinitions.size());

  // FIXME: Horrible hack. Store this somewhere more sane.
  TC.Context.LastCheckedExternalDefinition = currentExternalDef;
}

void swift::typeCheckExternalDefinitions(SourceFile &SF) {
  assert(SF.ASTStage == SourceFile::TypeChecked);
  auto &Ctx = SF.getASTContext();
  TypeChecker TC(Ctx);
  typeCheckFunctionsAndExternalDecls(TC);
}

void swift::performTypeChecking(SourceFile &SF, TopLevelContext &TLC,
                                unsigned StartElem) {
  if (SF.ASTStage == SourceFile::TypeChecked)
    return;

  // Make sure that name binding has been completed before doing any type
  // checking.
  performNameBinding(SF, StartElem);

  auto &Ctx = SF.getASTContext();
  TypeChecker TC(Ctx);
  auto &DefinedFunctions = TC.definedFunctions;
  
  // Lookup the swift module.  This ensures that we record all known protocols
  // in the AST.
  (void) TC.getStdlibModule(&SF);

  // Resolve extensions. This has to occur first during type checking,
  // because the extensions need to be wired into the AST for name lookup
  // to work.
  // FIXME: We can have interesting ordering dependencies among the various
  // extensions, so we'll need to be smarter here.
  // FIXME: The current source file needs to be handled specially, because of
  // private extensions.
  bool ImportsFoundationModule = false;
  auto FoundationModuleName = Ctx.getIdentifier("Foundation");
  SF.forAllVisibleModules([&](Module::ImportedModule import) {
    if (import.second->getName() == FoundationModuleName)
      ImportsFoundationModule = true;

    // FIXME: Respect the access path?
    for (auto file : import.second->getFiles()) {
      auto SF = dyn_cast<SourceFile>(file);
      if (!SF)
        continue;

      for (auto D : SF->Decls) {
        if (auto ED = dyn_cast<ExtensionDecl>(D)) {
          bindExtensionDecl(ED, TC);
          if (mayConformToKnownProtocol(ED) &&
              ED->getExtendedType()->getAnyNominal())
            TC.validateDecl(ED->getExtendedType()->getAnyNominal());
        } else if (auto nominal = dyn_cast<NominalTypeDecl>(D)) {
          if (mayConformToKnownProtocol(nominal))
            TC.validateDecl(nominal);
        }
      }
    }
  });

  // FIXME: Check for cycles in class inheritance here?

  // Type check the top-level elements of the source file.
  for (auto D : llvm::makeArrayRef(SF.Decls).slice(StartElem)) {
    if (isa<TopLevelCodeDecl>(D))
      continue;

    TC.typeCheckDecl(D, /*isFirstPass*/true);
  }

  // At this point, we can perform general name lookup into any type.

  // We don't know the types of all the global declarations in the first
  // pass, which means we can't completely analyze everything. Perform the
  // second pass now.

  bool hasTopLevelCode = false;
  for (auto D : llvm::makeArrayRef(SF.Decls).slice(StartElem)) {
    if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      hasTopLevelCode = true;
      // Immediately perform global name-binding etc.
      TC.typeCheckTopLevelCodeDecl(TLCD);
    } else {
      TC.typeCheckDecl(D, /*isFirstPass*/false);
    }
  }

  if (hasTopLevelCode) {
    TC.contextualizeTopLevelCode(TLC,
                           llvm::makeArrayRef(SF.Decls).slice(StartElem));
  }
  
  DefinedFunctions.insert(DefinedFunctions.end(),
                          TC.implicitlyDefinedFunctions.begin(),
                          TC.implicitlyDefinedFunctions.end());
  TC.implicitlyDefinedFunctions.clear();

  // If we're in REPL mode, inject temporary result variables and other stuff
  // that the REPL needs to synthesize.
  if (SF.Kind == SourceFileKind::REPL && !TC.Context.hadError())
    TC.processREPLTopLevel(SF, TLC, StartElem);

  typeCheckFunctionsAndExternalDecls(TC);

  // Verify that we've checked types correctly.
  SF.ASTStage = SourceFile::TypeChecked;

  // Emit an error if there is a declaration with the @objc attribute
  // but we have not imported the ObjectiveC module.
  if (Ctx.LangOpts.EnableObjCAttrRequiresObjCModule &&
      SF.Kind == SourceFileKind::Main &&
      StartElem == 0 &&
      SF.FirstObjCAttrLoc && !ImportsFoundationModule) {
    auto L = SF.FirstObjCAttrLoc.getValue();
    Ctx.Diags.diagnose(L, diag::objc_decl_used_without_required_module,
                       "objc", FoundationModuleName)
    .highlight(SourceRange(L));
  }

  // Verify the SourceFile.
  verify(SF);

  // Verify modules imported by Clang importer.
#ifndef NDEBUG
  if (SF.Kind != SourceFileKind::REPL)
    if (auto ClangLoader = TC.Context.getClangModuleLoader())
      ClangLoader->verifyAllModules();
#endif
}

namespace {
/// Add the 'final' property to decls when permitted
class TryAddFinal : ASTWalker {
  friend ASTWalker;

  Module *M;
  bool WholeModComp;

public:
  TryAddFinal(Module *Mod, bool WholeModuleCompilation)
    : M(Mod), WholeModComp(WholeModuleCompilation) {}

  void operator()(Decl *D) {
    D->walk(*this);
  }

private:
  std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
    return { false, S };
  }
  std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
    // ASTWalker skips VarDecls, picking them up in Patterns.
    return { true, P };
  }
  bool walkToTypeReprPre(TypeRepr *T) override { return false; }

  // Determine whether the 'dynamic' was inferred for this declaration and
  // all of the declarations it overrides.
  static bool isInferredDynamic(ValueDecl *ValD) {
    if (!ValD)
      return true;

    // If we have an accessor function, check whether the abstract storage
    // declaration itself has its dynamic inferred.
    if (auto func = dyn_cast<FuncDecl>(ValD)) {
      if (func->isAccessor()) {
        if (!isInferredDynamic(func->getAccessorStorageDecl()))
          return false;
      }
    }

    // Check whether this declaration is dynamic.
    if (auto dynamic = ValD->getAttrs().getAttribute<DynamicAttr>()) {
      // If 'dynamic' was implicit, check whether the overridden declaration
      // is also implicit.
      if (dynamic->isImplicit())
        return isInferredDynamic(ValD->getOverriddenDecl());

      return false;
    }

    return true;
  }

  bool walkToDeclPre(Decl *D) override {
    auto ValD = dyn_cast<ValueDecl>(D);
    if (!ValD)
      return true;

    // Constructors don't accept final as an attribute
    if (isa<ConstructorDecl>(ValD) || isa<DestructorDecl>(ValD))
      return true;

    // Already final (or invalid / not type checked)
    if (ValD->isFinal() || ValD->isInvalid() || !ValD->hasAccessibility())
      return false;

    // final cannot apply to dynamic functions, unless "dynamic" was
    // inferred to work around our inability to override methods in extensions
    // (see inferDynamic in TypeCheckDecl.cpp).
    bool removeDynamic = false;
    DynamicAttr *dynamicAttr = ValD->getAttrs().getAttribute<DynamicAttr>();
    if (dynamicAttr) {
      // If this 'dynamic' wasn't inferred, we cannot apply 'final'.
      if (!isInferredDynamic(ValD))
        return false;

      // Allow us to add 'final' to a dynamic function. We'll remove the
      // inferred 'dynamic' if we do add 'final'.
      removeDynamic = true;
    }

    // final can only be applied to private or internal. For internal, only if
    // we can see the entire module
    auto Access = ValD->getAccessibility();
    if (Access == Accessibility::Public)
      return true;
    if (Access == Accessibility::Internal && !WholeModComp)
      return true;

    if (auto ASD = dyn_cast<AbstractStorageDecl>(ValD)) {
      // We can add final if we're overridden and we're in a class
      if (!ASD->isOverridden() && isInClass(ASD->getDeclContext())) {
        addFinal(ASD);

        if (removeDynamic)
          ValD->getAttrs().removeAttribute(dynamicAttr);
      }
      return true;
    }

    if (auto AFD = dyn_cast<AbstractFunctionDecl>(ValD)) {
      // We can add final if we're overridden and we're in a class
      if (!AFD->isOverridden() && isInClass(AFD->getDeclContext())) {
        // FIXME: Remove this the below workaround no longer applies
        //
        // Work-around for a problem in how we override individual accessors: we
        // currently will consider a derived setter to be override even if the
        // base setter is not accessible to the derived class.
        //
        // For now, we work around it by not letting setters be final if the
        // property is not final
        if (auto FD = dyn_cast<FuncDecl>(AFD))
          if (FD->isSetter() && !FD->getAccessorStorageDecl()->isFinal())
            return true;

        addFinal(AFD);
        if (removeDynamic)
          ValD->getAttrs().removeAttribute(dynamicAttr);
      }
      return true;
    }

    // @objc on classes means that it can be arbitrarily subclassed, so we
    // can't do anything.
    if (auto CD = dyn_cast<ClassDecl>(ValD)) {
      if (CD->isObjC())
        return true;

      // TODO: Also add final to classes
      return true;
    }

    return true;
  }

  /// Add the final attribute to a decl
  void addFinal(ValueDecl *ValD) {
    // Do not add the 'final' attribute - see rdar://17890078
    // ValD->getAttrs().add(new (M->Ctx) FinalAttr(/*IsImplicit=*/true));
  }

  /// Whether we're a decl inside a class
  bool isInClass(DeclContext *DC) {
    auto DTIC = DC->getDeclaredTypeInContext();
    return DTIC && DTIC->getClassOrBoundGenericClass();
  }

};
} // end namespace

void swift::performWholeModuleChecks(Module *M,
                                     const SourceFile *PrimarySourceFile,
                                     bool WholeModuleComp) {
  TryAddFinal tryFinal(M, WholeModuleComp);
  for (auto File : M->getFiles())
    if (auto SF = dyn_cast<SourceFile>(File))
      if (WholeModuleComp || SF == PrimarySourceFile)
        for (auto D : SF->Decls)
          tryFinal(D);
}

bool swift::performTypeLocChecking(ASTContext &Ctx, TypeLoc &T,
                                   bool isSILType, DeclContext *DC,
                                   bool ProduceDiagnostics) {
  TypeResolutionOptions options;
  if (isSILType)
    options |= TR_SILType;

  if (ProduceDiagnostics) {
    return TypeChecker(Ctx).validateType(T, DC, options);
  } else {
    // Set up a diagnostics engine that swallows diagnostics.
    DiagnosticEngine Diags(Ctx.SourceMgr);
    return TypeChecker(Ctx, Diags).validateType(T, DC, options);
  }
}

/// Expose TypeChecker's handling of GenericParamList to SIL parsing.
bool swift::handleSILGenericParams(ASTContext &Ctx, GenericParamList *gp,
                                   DeclContext *DC,
                                   ArchetypeBuilder *builder) {
  return TypeChecker(Ctx).handleSILGenericParams(builder, gp, DC);
}

bool swift::typeCheckCompletionDecl(Decl *D) {
  auto &Ctx = D->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);
  TypeChecker TC(Ctx, Diags);

  TC.typeCheckDecl(D, true);
  return true;
}

bool swift::typeCheckCompletionContextExpr(ASTContext &Ctx, DeclContext *DC,
                                           Expr *&parsedExpr) {
  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, diags);
  TC.typeCheckExpression(parsedExpr, DC, Type(), Type(), /*discardedExpr=*/true,
                         FreeTypeVariableBinding::GenericParameters);
  
  return parsedExpr && !isa<ErrorExpr>(parsedExpr)
                    && parsedExpr->getType()
                    && !parsedExpr->getType()->is<ErrorType>();
}

bool swift::typeCheckAbstractFunctionBodyUntil(AbstractFunctionDecl *AFD,
                                               SourceLoc EndTypeCheckLoc) {
  auto &Ctx = AFD->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, Diags);
  return !TC.typeCheckAbstractFunctionBodyUntil(AFD, EndTypeCheckLoc);
}

bool swift::typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
  auto &Ctx = static_cast<Decl *>(TLCD)->getASTContext();

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(Ctx.SourceMgr);

  TypeChecker TC(Ctx, Diags);
  TC.typeCheckTopLevelCodeDecl(TLCD);
  return true;
}

static void deleteTypeCheckerAndDiags(LazyResolver *resolver) {
  DiagnosticEngine &diags = static_cast<TypeChecker*>(resolver)->Diags;
  delete resolver;
  delete &diags;
}

OwnedResolver swift::createLazyResolver(ASTContext &Ctx) {
  auto diags = new DiagnosticEngine(Ctx.SourceMgr);
  return OwnedResolver(new TypeChecker(Ctx, *diags),
                       &deleteTypeCheckerAndDiags);
}

void TypeChecker::diagnoseAmbiguousMemberType(Type baseTy,
                                              SourceRange baseRange,
                                              Identifier name,
                                              SourceLoc nameLoc,
                                              LookupTypeResult &lookup) {
  diagnose(nameLoc, diag::ambiguous_member_type, name, baseTy)
    .highlight(baseRange);
  for (const auto &member : lookup) {
    diagnose(member.first, diag::found_candidate_type,
             member.second);
  }
}
