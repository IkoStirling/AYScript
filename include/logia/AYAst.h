#pragma once
// AYAst.h - AST node definitions for Logia (S0)

#include "AYToken.h"
#include <memory>
#include <string>
#include <variant>
#include <vector>

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
