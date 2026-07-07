#pragma once
// AYAst.h - AST node definitions for Logia (S0) + type-attachment slots (S2)

#include "AYToken.h"
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ayt::reflect
{
class ITypeInfo;
class IFieldInfo;
} // namespace ayt::reflect

namespace ayt::script::logia
{

enum class LifecycleKind {
    OnStart,
    OnUpdate,
    OnDestroy,
};

class Expr;
class Stmt;
class ComponentDecl;
class Program;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

class Expr {
public:
    virtual ~Expr() = default;

    // ---- S2: SemanticAnalyzer stamps these fields in place ----
    // `resolvedType` is the static type of this expression after lookup
    // (nullptr means unresolved — codegen keeps S1 behavior in that case).
    // `resolvedField` is set on the leaf MemberExpr when the chain resolves
    // against a registered type.
    // `resolvedDecl` is set on IdentifierExpr pointing to the VarDeclStmt* /
    // Param* that introduced the name into scope (opaque to avoid extra
    // includes here).
    const ayt::reflect::ITypeInfo*   resolvedType  = nullptr;
    const ayt::reflect::IFieldInfo*  resolvedField = nullptr;
    const void*                      resolvedDecl  = nullptr;
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(ExprPtr left, Token op, ExprPtr right)
        : left(std::move(left)), op(op), right(std::move(right)) {}
    ExprPtr left;
    Token op;
    ExprPtr right;
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(Token op, ExprPtr operand)
        : op(op), operand(std::move(operand)) {}
    Token op;
    ExprPtr operand;
};

class CallExpr : public Expr {
public:
    CallExpr(ExprPtr callee, std::vector<ExprPtr> args)
        : callee(std::move(callee)), args(std::move(args)) {}
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(std::string name) : name(std::move(name)) {}
    std::string name;
};

class LiteralExpr : public Expr {
public:
    using Value = std::variant<std::monostate, bool, float, int, std::string>;
    explicit LiteralExpr(Value value) : value(std::move(value)) {}
    Value value;
};

class MemberExpr : public Expr {
public:
    MemberExpr(ExprPtr object, std::string member)
        : object(std::move(object)), member(std::move(member)) {}
    ExprPtr object;
    std::string member;
};

class IndexExpr : public Expr {
public:
    IndexExpr(ExprPtr object, ExprPtr index)
        : object(std::move(object)), index(std::move(index)) {}
    ExprPtr object;
    ExprPtr index;
};

class Stmt {
public:
    virtual ~Stmt() = default;
};

class ExprStmt : public Stmt {
public:
    explicit ExprStmt(ExprPtr expr) : expr(std::move(expr)) {}
    ExprPtr expr;
};

class VarDeclStmt : public Stmt {
public:
    VarDeclStmt(bool exported, std::string name, std::string typeName, ExprPtr initializer)
        : exported(exported),
          name(std::move(name)),
          typeName(std::move(typeName)),
          initializer(std::move(initializer)) {}
    bool exported = false;
    std::string name;
    std::string typeName;
    ExprPtr initializer;

    // ---- S2: SemanticAnalyzer resolves `typeName` to a registered type. ----
    // nullptr when unresolved (built-in or unregistered — analyzer reports
    // an error in the latter case before stamping null).
    const ayt::reflect::ITypeInfo* resolvedType = nullptr;
};

class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(ExprPtr value) : value(std::move(value)) {}
    ExprPtr value;
};

class IfStmt : public Stmt {
public:
    IfStmt(ExprPtr condition, std::vector<StmtPtr> thenBranch, std::vector<StmtPtr> elseBranch)
        : condition(std::move(condition)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}
    ExprPtr condition;
    std::vector<StmtPtr> thenBranch;
    std::vector<StmtPtr> elseBranch;
};

struct Param {
    std::string name;
    std::string typeName;
};

class LifecycleFuncDecl : public Stmt {
public:
    LifecycleFuncDecl(LifecycleKind kind, std::vector<Param> params, std::vector<StmtPtr> body)
        : kind(kind), params(std::move(params)), body(std::move(body)) {}
    LifecycleKind kind;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
};

class ComponentDecl {
public:
    ComponentDecl(std::string name, std::vector<StmtPtr> members)
        : name(std::move(name)), members(std::move(members)) {}
    std::string name;
    std::vector<StmtPtr> members;
};

class Program {
public:
    explicit Program(std::vector<std::unique_ptr<ComponentDecl>> components)
        : components(std::move(components)) {}
    std::vector<std::unique_ptr<ComponentDecl>> components;
};

const char* lifecycleKindName(LifecycleKind kind);

} // namespace ayt::script::logia
