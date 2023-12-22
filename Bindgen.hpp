#ifndef MOCKOTO_BINDGEN_HPP
#define MOCKOTO_BINDGEN_HPP

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream> //std::stringstream

#include "Config.hpp"
extern "C" {
#include "binders.h"
}

using namespace clang;

namespace mockoto {
const std::string prolog = R"rkt(

;; -----------------------------------------------------------------------------
;; WARNING: auto-generated code - Changes will be lost!
;; -----------------------------------------------------------------------------

(require ffi/unsafe)
(define _size_t _uint64)
(define _uintptr_t _uint64)
)rkt";

class BindgenVisitor : public RecursiveASTVisitor<BindgenVisitor> {
  ASTContext *Context;
  mockoto::Config Config;
  llvm::StringRef InFile;

  std::map<std::string, std::set<std::string>> dependsOn;
  std::map<std::string, std::string> definitions;
  std::set<std::string> ready;
  std::set<std::string> predefined;
  int count;

  void checkPending() {
    std::vector<std::string> to_erase;

    // scan all pending definitions and check if dependencies
    // are now complete
    for (auto i = dependsOn.begin(); i != dependsOn.end(); i++) {
      std::set<std::string> dependencies;
      for (auto j = i->second.begin(); j != i->second.end(); j++)
        // if (!definitions.count(*j) && predefined.find(*j) ==
        // predefined.end())
        if (ready.find(*j) == ready.end() &&
            predefined.find(*j) == predefined.end())
          dependencies.insert(*j);

      if (dependencies.empty()) {
        ready.insert(i->first);
        llvm::outs() << definitions[i->first] << "\n\n";
        to_erase.push_back(i->first);
      } else
        dependsOn[i->first] = dependencies;
    }

    // remove newly printed definitions
    for (auto k : to_erase)
      dependsOn.erase(k);
  }

  bool output(std::stringstream &ss) {
    llvm::outs() << ss.str() << "\n\n";
    return true;
  }

public:
  ~BindgenVisitor() {
    for (auto i = dependsOn.begin(); i != dependsOn.end(); i++) {
      if (i->second.size() == 1 && i->second.find("_void") != i->second.end()) {
        llvm::outs() << ";; postponed\n";
        llvm::outs() << definitions[i->first] << "\n\n";
        ready.insert(i->first);
        checkPending();
        continue;
      }
      llvm::outs() << ";; pending:   " << i->first << "\n";
      llvm::outs() << ";; missing ->";
      for (auto j = i->second.begin(); j != i->second.end(); j++)
        llvm::outs() << " " << *j;

      llvm::outs() << "\n\n\n";
    }
  }

  std::map<uintptr_t, std::string> anonymous;
  int anonCount;

public:
  explicit BindgenVisitor(ASTContext *Context, mockoto::Config &Config,
                          llvm::StringRef InFile)
      : Context(Context), Config(Config), InFile(InFile) {

    std::stringstream binders;
    binders.write(reinterpret_cast<char *>(binders_rkt), binders_rkt_len);

    llvm::outs() << binders.str();
    llvm::outs() << prolog << "\n";
    predefined.insert("_size_t");
    predefined.insert("_uintptr_t");
  }

  bool VisitNamedDecl(NamedDecl *NamedDecl) {
    SourceManager &sm = Context->getSourceManager();
    std::string location = NamedDecl->getLocation().printToString(sm);
    checkPending();

    for (auto pfx : Config.excludePatterns)
      if (location.find(pfx) != std::string::npos)
        return true;

    // remove column number from location string
    size_t from = location.find(":", location.find(":") + 1);
    location = location.erase(from, location.length() - from);

    std::string name = NamedDecl->getQualifiedNameAsString();
    std::stringstream definition;
    std::set<std::string> dependencies;

    if (name.find("(anonymous") != std::string::npos) {
      uintptr_t ptr = (uintptr_t)NamedDecl;
      if (anonymous.find(ptr) == anonymous.end()) {
        anonymous[ptr] = "anon_" + std::to_string(++anonCount);
      }
      name = anonymous[ptr];
    }

    if (const EnumDecl *decl = dyn_cast<EnumDecl>(NamedDecl)) {
      name = "_" + name;
      definition << ";; record " << name << "\n";
      if (Config.printSourcePath)
        definition << ";; " << location << "\n";
      if (definitions.count(name)) {
        definition << ";; " << name << " already defined";
        return output(definition);
      }
      definition << "(define " << name << "\n\t(_enum '(";

      for (auto it = decl->enumerator_begin(); it != decl->enumerator_end();
           it++) {
        auto indent = it == decl->enumerator_begin() ? "" : "\n\t\t";
        std::string tag = it->getNameAsString();
        definition << indent << tag << " = " << it->getInitVal().getExtValue();
      }
      definition << "\n)))\n\n";

      definition << "(define " << name << "-domain\n\t'(";

      for (auto it = decl->enumerator_begin(); it != decl->enumerator_end();
           it++) {
        auto indent = it == decl->enumerator_begin() ? "" : "\n\t\t";
        std::string tag = it->getNameAsString();
        definition << indent << tag;
      }
      definition << "\n))";
    } else if (const RecordDecl *decl = dyn_cast<RecordDecl>(NamedDecl)) {
      std::string oname = name;
      name = "_" + name;
      definition << ";; record " << name << "\n";
      if (Config.printSourcePath)
        definition << ";; " << location << "\n";
      if (!decl->isCompleteDefinition()) {
        definition << ";; incomplete definition\n";
        definition << "(define " << name << " _void)";
        definition << "(define " << name << "-pointer _pointer)";
        // return output(definition);
        dependencies.insert("_void");
        goto end;
      }
      if (decl->isAnonymousStructOrUnion()) {
        definition << ";; do not support anonymous struct or union\n\n";
        return output(definition);
      }

      if (false && definitions.count(name)) {
        definition << ";; " << name << " already defined\n\n";
        return output(definition);
      }

      bool isUnion = decl->isUnion();
      if (isUnion)
        definition << "(define " << name << "\n\t(_union ";
      else
        definition << "(define-cstruct " << name << "\n\t(";

      for (auto it = decl->field_begin(); it != decl->field_end(); it++) {
        QualType type = it->getType();
        type.removeLocalFastQualifiers();

        auto fieldName = it->getNameAsString();
        auto p = getTypeAsString(type, dependencies, name);
        if (isUnion)
          definition << (it == decl->field_begin() ? "" : "\n\t\t") //
                     << (p.first ? "" : ";; error: ")               //
                     << p.second;
        else
          definition << (it == decl->field_begin() ? "" : "\n\t ") //
                     << (p.first ? "" : ";; error: ")              //
                     << "[" << fieldName << " " << p.second << "]";
      }
      definition << "\n))";

      if (!isUnion) {
        definition << "\n"                                                    //
                   << "(define (alloc-" << oname << ")\n"                     //
                   << "\t(cast (malloc 'raw (ctype-sizeof " << name << "))\n" //
                   << "\t      _pointer\n"                                    //
                   << "\t      " << name << "-pointer))";
      }
    } else if (const TypedefNameDecl *decl =
                   dyn_cast<TypedefNameDecl>(NamedDecl)) {
      name = "_" + name;
      definition << ";; typedef " << name << "\n";
      if (Config.printSourcePath)
        definition << ";; " << location << "\n";
      if (definitions.count(name)) {
        definition << ";; " << name << " already defined\n\n";
        return output(definition);
      }

      auto type = decl->getUnderlyingType();
      if (type->isFunctionPointerType()) {
        auto fptype = type->getPointeeType()->getAs<clang::FunctionType>();
        auto ftype = fptype->getAs<clang::FunctionProtoType>();
        auto ret = getTypeAsString(ftype->getReturnType(), dependencies);
        if (!ret.first)
          definition << ";; error: ";
        definition << "(define " << name << "\n\t(_fun ";
        for (int i = 0; i < ftype->getNumParams(); i++) {
          auto p = getTypeAsString(ftype->getParamTypes()[i], dependencies);
          definition << (p.first ? "" : ";; error: ") //
                     << (i == 0 ? "" : " ") << p.second;
        }
        definition << " -> " << ret.second << "))";
      } else {
        auto p = getTypeAsString(decl->getUnderlyingType(), dependencies);
        definition << (p.first ? "" : ";; error: ") //
                   << "(define " << name << " " << p.second << ")";
      }
    } else if (const FunctionDecl *decl = dyn_cast<FunctionDecl>(NamedDecl)) {
      name = "_" + name;
      definition << ";; function " << name << "\n";
      if (Config.printSourcePath)
        definition << ";; " << location << "\n";
      if (definitions.count(name)) {
        definition << ";; " << name << " already defined\n\n";
        return output(definition);
      }
      if (!decl->hasPrototype()) {
        definition << ";; function without prototype\n";
        return output(definition);
      }

      auto ftype = decl->getType()->getAs<clang::FunctionProtoType>();
      auto ret = getTypeAsString(ftype->getReturnType(), dependencies);
      if (!ret.first)
        definition << ";; error: ";
      definition << "(define " << name << "\n\t(_fun ";
      for (int i = 0; i < ftype->getNumParams(); i++) {
        auto p = getTypeAsString(ftype->getParamTypes()[i], dependencies);
        definition << (p.first ? "" : ";; error: ") //
                   << (i == 0 ? "" : " ") << p.second;
      }
      definition << " -> " << ret.second << "))";
    }
  end:
    if (definition.str() != "") {
      definitions[name] = definition.str();
      dependsOn[name] = dependencies;
    }
    return true;
  }

private:
  std::pair<bool, std::string>
  getTypeAsString(QualType type, std::set<std::string> &dependencies,
                  std::string parent = "") {
    std::string typeStr;
    bool isPointer = false;
    bool isFunctionPointer = false;
    bool ok = true;
    auto otype = type;
    bool wasChar = false;

    if (type->isPointerType()) {
      type = type->getPointeeType();
      type.removeLocalFastQualifiers();
      isPointer = true;
    }

    if (otype->isFunctionPointerType()) {
      isFunctionPointer = true;
      auto ftype = type->getAs<clang::FunctionProtoType>();
      auto ret = getTypeAsString(ftype->getReturnType(), dependencies, parent);
      if (!ret.first)
        typeStr += ";; error: ";
      typeStr += "(_fun ";
      for (int i = 0; i < ftype->getNumParams(); i++) {
        auto p =
            getTypeAsString(ftype->getParamTypes()[i], dependencies, parent);
        typeStr += (p.first ? "" : ";; error: ");
        typeStr += (i == 0 ? "" : " ") + p.second;
      }
      typeStr += " -> " + ret.second + ")";

    } else if (type->isArrayType()) {
      auto arrayType = type->getAsArrayTypeUnsafe();
      auto elemType = arrayType->getElementType();
      uint64_t width = Context->getTypeInfo(type).Width;
      uint64_t esize = Context->getTypeInfo(elemType).Width;
      uint64_t count = width / esize;
      ok = ok && (count > 0);
      auto p = getTypeAsString(elemType, dependencies, parent);
      typeStr =
          "(_array/vector " + p.second + " " + std::to_string(count) + ")";
    } else if (type->isUnionType()) {
      const RecordType *rt = type->getAsUnionType();
      typeStr = "_" + rt->getDecl()->getQualifiedNameAsString();
      if (typeStr != parent)
        dependencies.insert(typeStr);
    } else if (type->isEnumeralType()) {
      typeStr = "_" + type->getAsTagDecl()->getQualifiedNameAsString();
      if (typeStr != parent)
        dependencies.insert(typeStr);
    } else if (type->isStructureType()) {
      const RecordType *rt = type->getAsStructureType();
      typeStr = "_" + rt->getDecl()->getQualifiedNameAsString();
      if (typeStr != parent)
        dependencies.insert(typeStr);
    } else {
      typeStr = type.getAsString();
      std::map<std::string, std::string> m = //
          {
              // convert known types
              {"_Bool", "_bool"},          //
              {"unsigned int", "_uint32"}, //
              {"int", "_int32"},           //
              {"char", "_byte"},           //
              {"uint8_t", "_uint8"},       //
              {"uint16_t", "_uint16"},     //
              {"uint32_t", "_uint32"},     //
              {"uint64_t", "_uint64"},     //
              {"void", "_void"},           //
          };
      if (typeStr == "char")
        wasChar = true;
      if (m[typeStr] != "")
        typeStr = m[typeStr];
      else {
        typeStr = "_" + typeStr;

        if (typeStr != parent)
          dependencies.insert(typeStr);
      }
    }

    if (isPointer && !isFunctionPointer)
      typeStr = (typeStr == "_void" || typeStr == parent)
                    ? "_pointer"
                    : ((type->isUnionType() || type->isStructureType())
                           ? "(_or-null " + typeStr + "-pointer)"
                       : wasChar ? "_string"
                                 : "(_cpointer " + typeStr + ") ");
    return std::make_pair(ok, typeStr);
  }
};

} // namespace mockoto

#endif
