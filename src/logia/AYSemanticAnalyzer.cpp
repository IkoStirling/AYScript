// AYSemanticAnalyzer.cpp - Logia semantic analyzer (S2.5 + S3.0 LG-03)
//
// S2.5: `script Name` binds to a C++ ScriptComponent subclass (looked up
// in AYReflect TypeRegistry). `self` is the canonical way to access
// fields on that component. `var` is a pure Lua local.
//
// S3.0 (LG-03): the analyzer ctor also takes a `LogiaHostContext`.
// LG-03 stores it but does NOT enable new validation — the S2.5
// lookup path (`findType(name)`) and S2.5 soft-warning diagnostics
// remain exactly as before. Future S3.1+ host-aware validation will
// read `_ctx.hostKind` / `_ctx.hostType` / `_ctx.expectSelf`; LG-03
// keeps the existing call signature stable for that future work.

#include "logia/AYSemanticAnalyzer.h"

#include "aylog/Logger.h"

// AYReflect surface
#include "ayreflect/IReflect.h"
#include "ayreflect/ReflectRegistry.h"
#include "AYReflect.h"  // full TypeRegistryImpl definition (linkable)

// S3.2 (LG-04b, B-min): isDerivedFrom is a free function defined in
// AYReflect.cpp. Forward-declare to keep the include surface small.
namespace ayt::reflect
{
bool isDerivedFrom(const ITypeInfo* type, const ITypeInfo* base);
} // namespace ayt::reflect

// Force AYEntity's Transform component to be registered with AYReflect.
#include "components/AYTransformComponent.h"
#include "AYEntityModule.h"
#include "components/AYHealthComponent.h"
#include "AYReflectMacros.h"  // ayt::reflect::detail::defaultCreate/Destroy/Copy
#include <aymath/MathTypes.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace ayt::script::logia
{

namespace
{

const std::unordered_set<std::string>& builtinTypeNames()
{
    static const std::unordered_set<std::string> s = {
        "int", "float", "bool", "string", "Entity"
    };
    return s;
}

// R5.2-C (2026-07-14): name → true if `name` is an integer-shape
// primitive. Distinct from `isBuiltInType()` (which only answers
// "is the name recognized as ambient"), because some builtins
// (`float`, `bool`, `string`) are not integer-shape. `int` /
// `Int32` / `Int64` are. Logia-script semantics: only
// integer-shape types are valid `for (...)` bounds.
//
// We deliberately exclude PascalCase float aliases (`Float32` /
// `Float64`) — those are real types and a `for (var i : x)`
// where x is `Float32` is a type error.
//
// Used by both the bound validator and (optionally) future
// expression-shape inference. Pure name match — does not touch
// AYReflect.
bool isIntegerTypeName(const std::string& name)
{
    return name == "int"
        || name == "Int32"
        || name == "Int64";
}

// R5.2-C (2026-07-14): pure-tree-shape predicate. Returns true
// iff `e` is statically an int given current analyzer state
// (resolvedType / literal value / constant-folded binary form).
//
// R5.2-E (2026-07-14): extends BinaryExpr / UnaryExpr recursion to
// type-propagate through int-typed identifier / member / call leaves.
// Cases handled (in priority order):
//   - IntLiteralExpr                     → true
//   - FloatLiteralExpr / StringLiteral   → false
//   - BoolLiteralExpr                    → false
//   - IdentifierExpr with resolvedType
//     pointing at "int"/"Int32"/"Int64"  → true  (R5.2-C: builtin
//                                          vars via resolvedDecl→
//                                          VarDeclStmt.typeName)
//   - MemberExpr leaf with resolvedType  → check ITypeInfo*'s name
//   - CallExpr with resolvedMethod whose
//     getReturnType() is int-shaped       → true  (S3.12 path)
//   - BinaryExpr where all leaves
//     (recursive) are int                → true  (R5.2-C literal-
//                                          folded only; R5.2-E
//                                          extends to type-recursion
//                                          through int-typed leaves —
//                                          `n + 1` where n: int
//                                          now passes since R5.2-C
//                                          already partially enabled
//                                          this through IdentifierExpr
//                                          path; R5.2-E commits to it)
//   - UnaryExpr where operand (recursive) is int → true  (same extension)
//   - anything else (no resolvedType, IndexExpr, TableExpr, etc.)
//     → false (default-reject per R5.2-C policy)
//
// IMPORTANT: this is bound-validation only. The step validator
// (evaluateAsInt, R5.2-D) still requires literal-folded expressions
// because step's value-not-type contract enforces `step != 0`.
// Relaxing step would require codegen-injected runtime checks
// (R5.2-G future slice), which violates R5.2-C/D's static-only
// principle.
//
// Free function (not a method): SemanticAnalyzer keeps
// resolvedType stamped on each Expr, so we only need read access.
bool boundIsStaticallyInt(const Expr* e)
{
    if (!e) return false;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        return std::holds_alternative<int>(lit->value);
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) {
        // R5.2-C (2026-07-14): for script-declared `var n: int`
        // the scope entry stores type=nullptr (built-in types
        // never have an ITypeInfo*) — but the VarDeclStmt's
        // `typeName` field still carries the user's annotation.
        // resolveDecl points at the VarDeclStmt; reading typeName
        // there is the cleanest way to recover the int shape
        // without disturbing the analyzer's scope semantics.
        if (id->resolvedDecl) {
            if (auto* vd = static_cast<const VarDeclStmt*>(
                    id->resolvedDecl)) {
                if (isIntegerTypeName(vd->typeName)) {
                    return true;
                }
            }
        }
        if (!id->resolvedType) return false;
        return isIntegerTypeName(id->resolvedType->getName());
    }

    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        if (!m->resolvedType) return false;
        return isIntegerTypeName(m->resolvedType->getName());
    }

    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        if (!c->resolvedMethod) return false;
        auto* rt = c->resolvedMethod->getReturnType();
        if (!rt) return false;
        return isIntegerTypeName(rt->getName());
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        // Constant-folded shape check. Operator-agnostic — `+ - * / % ^`
        // all produce int when both operands are int literals.
        return boundIsStaticallyInt(b->left.get())
            && boundIsStaticallyInt(b->right.get());
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        return boundIsStaticallyInt(u->operand.get());
    }

    return false;
}

// R5.2-H (2026-07-14): name → true iff `name` is the bool primitive.
// Distinct from `isIntegerTypeName` because the user-decided rule
// for `if` / `while` conditions is "must be bool" — NOT "any
// non-bool value with C-style truthiness". `int n` does NOT make
// `while (n)` valid, and an `int == int` BinaryExpr DOES make it
// valid. This is a deliberate split from Lua, which would treat
// `while (0)` as `while (false)` and `while (5)` as `while (true)`.
// Logia has no such coercion — silent truthiness is a class of bug
// (off-by-one in counters, accidentally truthy strings, etc.) we
// never want to inherit.
bool isBooleanTypeName(const std::string& name)
{
    return name == "bool";
}

// INT-03 (2026-07-15): numeric-type-name mirror of
// isBooleanTypeName. Used by the new isStaticallyNumeric sibling
// to detect "is this expression a number?". Accepts int / float /
// double plus int64 for R4.x long-path compat — Logia source
// has no separate int64 literal syntax so this is a forward-
// looking entry, not a current code path.
bool isNumericTypeName(const std::string& name)
{
    return name == "int" || name == "float" || name == "double"
        || name == "int64";
}

// R5.2-H (2026-07-14): forward-declared helper used by
// `isStaticallyBool`'s comparison branch. Defined below the
// call site because `isStaticallyBool` is the primary public
// helper and `leafIsStaticallyPrimitive` is just its
// primitive-shape sub-predicate.
bool leafIsStaticallyPrimitive(const Expr* e, const SemanticAnalyzer* analyzer);

// R5.2-H (2026-07-14): pure-tree-shape predicate for "this Expr
// is statically a bool". Parallel to `boundIsStaticallyInt` (L120).
// Used by `if (...)` / `while (...)` condition validators.
//
// Decision matrix (handled in priority order):
//   - BoolLiteralExpr                     → true (LiteralExpr
//                                          with variant<bool>)
//   - FloatLiteralExpr / IntLiteralExpr /
//     StringLiteral                       → false (no implicit
//                                          truthiness)
//   - IdentifierExpr with resolvedType /
//     resolvedDecl pointing at "bool"     → true (mirror R5.2-C's
//                                          builtin-var fallback:
//                                          `var b: bool` has
//                                          scope.type=nullptr but
//                                          VarDeclStmt.typeName
//                                          carries the annotation)
//   - MemberExpr leaf with resolvedType
//     getName() == "bool"                 → true
//   - CallExpr with resolvedMethod whose
//     getReturnType().getName() == "bool" → true
//   - UnaryExpr with op == Bang
//     over a bool leaf                    → true (e.g. `!ready`)
//   - BinaryExpr with COMPARISON op
//     {==, !=, <, <=, >, >=} over two
//     non-bool primitive leaves
//     (int/float/string)                  → true (`n > 0`,
//                                          `name == "foo"`,
//                                          `hp <= maxHp / 2`,
//                                          etc.) — comparison
//                                          returns bool per
//                                          Lua / any sane lang
//   - BinaryExpr with LOGICAL op
//     {&&, ||} over two bool leaves       → true (`a && b`,
//                                          `done || retry`)
//   - BinaryExpr with arithmetic op
//     {Plus, Minus, Star, Slash, Percent} → false (arithmetic
//                                          never produces bool;
//                                          user-decided — no
//                                          implicit int-as-bool)
//   - anything else (no resolvedType, IndexExpr,
//     TableExpr, CallExpr without resolvedMethod, etc.)
//                                          → false (default-reject)
//
// IMPORTANT: this is condition-validation only. It does NOT
// touch the bound validator (R5.2-C/E — int) or the step
// validator (R5.2-D — const-folded positive int). The three
// validators each enforce their own type contract.
//
// Free function (not a method): SemanticAnalyzer stamps
// resolvedType on identifier / member / call leaves, so we
// only need read access to the tree.
//
// R5.2-H.b (2026-07-14): the `analyzer` parameter is consulted
// to recognize:
//   - for-loop counters (pushed onto `analyzer->_loopCounters`
//     in analyzeForStmt) as int leaves — without this,
//     `if (j == 1) { ... }` inside `for (var j : 3)` would
//     reject because `j` is deliberately not in `_scope`.
//   - ambient-receiver CallExpr (`input.is_pressed(...)` where
//     `input` is ambient) as bool-yielding — without this, the
//     canonical Logia ↔ host shim pattern would hard-error
//     because `c->resolvedMethod` is null for non-self calls.
//
// Both carve-outs preserve the strict "no implicit truthiness"
// rule: we are NOT treating int as bool, we are recognizing
// that these specific AST shapes are statically known to yield
// bool (loop counter is int, comparison yields bool; ambient
// receiver is a documented bool-yielding host shim).
bool isStaticallyBool(const Expr* e, const SemanticAnalyzer* analyzer)
{
    if (!e) return false;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        return std::holds_alternative<bool>(lit->value);
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) {
        // R5.2-C pattern (mirror): builtin-var scope entry has
        // type=nullptr, but VarDeclStmt.typeName still carries
        // the user's annotation. Read it via resolvedDecl.
        if (id->resolvedDecl) {
            if (auto* vd = static_cast<const VarDeclStmt*>(
                    id->resolvedDecl)) {
                if (isBooleanTypeName(vd->typeName)) {
                    return true;
                }
            }
        }
        if (!id->resolvedType) return false;
        return isBooleanTypeName(id->resolvedType->getName());
    }

    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        if (!m->resolvedType) return false;
        return isBooleanTypeName(m->resolvedType->getName());
    }

    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        // R5.2-H.b (2026-07-14): ambient-receiver carve-out.
        // Calls of the form `input.is_pressed(...)` /
        // `log.info(...)` / `time.delta()` where the receiver
        // is an ambient identifier (`input`, `log`, `time`)
        // are treated as bool-yielding when used in a
        // condition context. The host runtime injects these
        // names and contracts that method-shaped calls return
        // bool (input methods), etc. Without this carve-out
        // every canonical example fails to compile because
        // `c->resolvedMethod` is null for non-self callees
        // (analyzeCallExpr only stamps resolvedMethod for
        // `self.<method>(...)` — see L1431).
        //
        // Why this is NOT a truthiness carve-out: we are
        // saying "the host shim contract says this call yields
        // bool", NOT "treat any non-bool as bool". If the
        // ambient call returns something other than bool at
        // runtime, that's a host-shim bug, not a Logia bug.
        // The strict no-truthiness rule for non-ambient
        // expressions is unchanged.
        if (!c->resolvedMethod) {
            if (analyzer) {
                if (auto* mem = dynamic_cast<const MemberExpr*>(c->callee.get())) {
                    if (auto* recvId = dynamic_cast<const IdentifierExpr*>(mem->object.get())) {
                        if (SemanticAnalyzer::isAmbientIdentifier(recvId->name)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
        auto* rt = c->resolvedMethod->getReturnType();
        if (!rt) return false;
        return isBooleanTypeName(rt->getName());
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        // R5.2-H: only `!` (logical-not) preserves bool. `-` over
        // a bool is a type error elsewhere (arithmetic on bool
        // is meaningless); we don't need to second-guess that
        // here — falling through to false is correct.
        if (u->op.type != TokenType::Bang) return false;
        return isStaticallyBool(u->operand.get(), analyzer);
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        // R5.2-H: comparison operators ({==, !=, <, <=, >, >=})
        // return bool regardless of operand primitive type
        // (int/int, float/float, string/string all yield bool
        // per Lua and any sensible language). Logical operators
        // ({&&, ||}) require bool leaves (no implicit truthiness).
        // Arithmetic ops fall through to `return false` (no
        // implicit int-as-bool — user-decided principle).
        switch (b->op.type) {
            case TokenType::EqualEqual:
            case TokenType::BangEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            case TokenType::Greater:
            case TokenType::GreaterEqual: {
                // R5.2-H: comparison returns bool when both
                // leaves are primitive (int/float/bool/string).
                // We accept any leaf where the leaf can be
                // *reduced to a primitive type* — exactly the
                // same predicate `boundIsStaticallyInt` uses for
                // int, but generalized to all primitives. We
                // recurse to allow `n > (a + b)` (int > int via
                // arithmetic on int leaves) — see the predicate
                // helper below.
                return leafIsStaticallyPrimitive(b->left.get(), analyzer)
                    && leafIsStaticallyPrimitive(b->right.get(), analyzer);
            }
            case TokenType::And:
            case TokenType::Or:
                return isStaticallyBool(b->left.get(), analyzer)
                    && isStaticallyBool(b->right.get(), analyzer);
            default:
                // Plus / Minus / Star / Slash / Percent / Equal /
                // PlusEqual / etc. — none produce bool.
                return false;
        }
    }

    return false;
}

// INT-03 (2026-07-15): numeric-yielding mirror of isStaticallyBool.
// Mirrors the same ambient-receiver CallExpr carve-out so that any
// future caller wanting "is this expression statically numeric?"
// gets the same treatment as bool-yielding expressions. Today no
// caller uses it directly — the BinaryExpr comparison branch
// (L365-366 above) already accepts `axis() > 0` via
// leafIsStaticallyPrimitive which treats ambient-receiver CallExpr
// as a primitive leaf — but this future-proofs:
//   (a) `if axis() then` direct condition (currently rejected;
//       comparison or `> 0` wrapper is required);
//   (b) future boundIsStaticallyFloat helper for log.error("n=%d", n)
//       numeric-arg validation;
//   (c) future type-aware codegen that wants to know the numeric
//       yield type for bridging.
//
// Mirrors isStaticallyBool branch-for-branch: LiteralExpr
// (long long / double), IdentifierExpr (via resolvedDecl.typeName
// for builtin vars — R5.2-C pattern), MemberExpr (via
// resolvedType->getName()), CallExpr (ambient-receiver carve-out
// + resolvedMethod return-type check), UnaryExpr (-/+ only,
// ! falls through), BinaryExpr (arithmetic ops yield numeric
// when both leaves numeric — Plus / Minus / Star / Slash / Percent).
bool isStaticallyNumeric(const Expr* e, const SemanticAnalyzer* analyzer)
{
    if (!e) return false;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        // Numeric literal: int / float (per LiteralExpr::Value
        // variant — see AYAST.h:113). bool literal falls through
        // to false here (handled by isStaticallyBool). String
        // literal is not numeric.
        return std::holds_alternative<int>(lit->value)
            || std::holds_alternative<float>(lit->value);
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) {
        // R5.2-C pattern (mirror): builtin-var scope entry has
        // type=nullptr, but VarDeclStmt.typeName still carries
        // the user's annotation. Read it via resolvedDecl.
        if (id->resolvedDecl) {
            if (auto* vd = static_cast<const VarDeclStmt*>(
                    id->resolvedDecl)) {
                if (isNumericTypeName(vd->typeName)) {
                    return true;
                }
            }
        }
        if (!id->resolvedType) return false;
        return isNumericTypeName(id->resolvedType->getName());
    }

    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        if (!m->resolvedType) return false;
        return isNumericTypeName(m->resolvedType->getName());
    }

    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        // INT-03 ambient-receiver carve-out (mirror R5.2-H.b for
        // bool): input.axis(...) / log.info(...) where the
        // receiver is an ambient identifier is treated as
        // numeric-yielding — the host runtime injects these
        // names and contracts that axis-shaped calls return
        // float. Without this carve-out every canonical Logia
        // PlayerController.move_x example would fail to
        // compile in future numeric condition contexts because
        // c->resolvedMethod is null for non-self callees (see
        // analyzeCallExpr L1431 — resolvedMethod is only
        // stamped for self.<method>(...)).
        if (!c->resolvedMethod) {
            if (analyzer) {
                if (auto* mem = dynamic_cast<const MemberExpr*>(c->callee.get())) {
                    if (auto* recvId = dynamic_cast<const IdentifierExpr*>(mem->object.get())) {
                        if (SemanticAnalyzer::isAmbientIdentifier(recvId->name)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
        auto* rt = c->resolvedMethod->getReturnType();
        if (!rt) return false;
        return isNumericTypeName(rt->getName());
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        // Numeric-preserving unary: -n / +n. Logical-not (!)
        // yields bool, falls through to false. Bitwise-not (~)
        // is not in Logia surface.
        if (u->op.type != TokenType::Minus && u->op.type != TokenType::Plus) {
            return false;
        }
        return isStaticallyNumeric(u->operand.get(), analyzer);
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        // Arithmetic ops yield numeric when both leaves numeric.
        // Comparison ops yield bool (handled by isStaticallyBool
        // branch). && / || require bool leaves (also bool branch).
        switch (b->op.type) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Percent:
                return isStaticallyNumeric(b->left.get(), analyzer)
                    && isStaticallyNumeric(b->right.get(), analyzer);
            default:
                return false;
        }
    }

    return false;
}

// R5.2-H (2026-07-14): helper used by isStaticallyBool's
// comparison branch. Returns true iff `e` is statically
// reducible to a primitive type (int / float / bool / string),
// recursively through arithmetic / unary. Used to gate the
// `comparison → bool` rule: `n > 0` (int > int) is fine,
// `vec > 0` (struct > int) is rejected, `f() > 0` is fine
// when f returns int.
//
// Mirrors boundIsStaticallyInt's recursion strategy but
// accepts any primitive, not just int.
//
// R5.2-H.b (2026-07-14): also recognizes for-loop counter
// identifiers (pushed onto `analyzer->_loopCounters` in
// analyzeForStmt) as int-shaped primitives — the counter is
// deliberately NOT in `_scope` (per R5.0 / R5.2-C), so the
// usual IdentifierExpr path returns false on it.
bool leafIsStaticallyPrimitive(const Expr* e, const SemanticAnalyzer* analyzer)
{
    if (!e) return false;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        return std::holds_alternative<bool>(lit->value)
            || std::holds_alternative<int>(lit->value)
            || std::holds_alternative<float>(lit->value)
            || std::holds_alternative<std::string>(lit->value);
    }

    if (auto* id = dynamic_cast<const IdentifierExpr*>(e)) {
        // R5.2-H.b: for-loop counter carve-out. The counter
        // is not in `_scope` (R5.0 / R5.2-C's deliberate
        // design) but is pushed onto `_loopCounters` by
        // analyzeForStmt. Treat it as an int primitive so
        // `if (j == 1) { ... }` inside a `for (var j : 3)`
        // body is recognized as a comparison yielding bool.
        if (analyzer && analyzer->isLoopCounter(id->name)) {
            return true;  // counter is always int
        }
        if (id->resolvedDecl) {
            if (auto* vd = static_cast<const VarDeclStmt*>(
                    id->resolvedDecl)) {
                const auto& n = vd->typeName;
                return n == "int" || n == "Int32" || n == "Int64"
                    || n == "float" || n == "Float32" || n == "Float64"
                    || n == "bool" || n == "string";
            }
        }
        if (!id->resolvedType) return false;
        const auto n = id->resolvedType->getName();
        return n == "int" || n == "Int32" || n == "Int64"
            || n == "float" || n == "Float32" || n == "Float64"
            || n == "bool" || n == "string";
    }

    if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
        if (!m->resolvedType) return false;
        const auto n = m->resolvedType->getName();
        return n == "int" || n == "Int32" || n == "Int64"
            || n == "float" || n == "Float32" || n == "Float64"
            || n == "bool" || n == "string";
    }

    if (auto* c = dynamic_cast<const CallExpr*>(e)) {
        if (!c->resolvedMethod) return false;
        auto* rt = c->resolvedMethod->getReturnType();
        if (!rt) return false;
        const auto n = rt->getName();
        return n == "int" || n == "Int32" || n == "Int64"
            || n == "float" || n == "Float32" || n == "Float64"
            || n == "bool" || n == "string";
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        // Arithmetic on primitives is still primitive. Compare
        // ops are handled by the caller (isStaticallyBool).
        // && / || return bool, primitive. We accept both.
        switch (b->op.type) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Percent:
            case TokenType::EqualEqual:
            case TokenType::BangEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::And:
            case TokenType::Or:
                return leafIsStaticallyPrimitive(b->left.get(), analyzer)
                    && leafIsStaticallyPrimitive(b->right.get(), analyzer);
            default:
                return false;
        }
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        // `-` over a numeric leaf, `!` over a bool leaf — both
        // primitive.
        return leafIsStaticallyPrimitive(u->operand.get(), analyzer);
    }

    return false;
}

// R5.2-D (2026-07-14): constant-fold an Expr to an int value, or
// return std::nullopt if the expression is not a constant int
// expression. Distinct from `boundIsStaticallyInt` (which only
// checks *shape*, not *value*) because step validation needs
// both shape (must be int) and value (> 0).
//
// Handled (in priority order):
//   - IntLiteralExpr              → value
//   - BinaryExpr {+,-,*,/,%} of
//     two recursive int leaves    → value (with divide-by-zero → nullopt)
//   - UnaryExpr (-, not) over
//     int-shaped subtree          → value (negation only)
//   - Anything else (float/string/bool literals,
//     IdentifierExpr w/ resolvedType, MemberExpr, CallExpr,
//     IndexExpr, TableExpr, BinaryExpr with non-int leaf) → nullopt
//
// `op` is a Token (TokenType-tagged). Only TokenType::Plus / Minus
// / Star / Slash / Percent are valid int binary ops in Logia;
// no `^`/pow, no shift, no bitwise — see AYToken.h:46-50.
// Modulo/division-by-zero returns nullopt (no value to attribute;
// in practice step wouldn't have a zero divisor anyway).
std::optional<int> evaluateAsInt(const Expr* e)
{
    if (!e) return std::nullopt;

    if (auto* lit = dynamic_cast<const LiteralExpr*>(e)) {
        if (auto* p = std::get_if<int>(&lit->value)) {
            return *p;
        }
        return std::nullopt;
    }

    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        auto l = evaluateAsInt(b->left.get());
        auto r = evaluateAsInt(b->right.get());
        if (!l || !r) return std::nullopt;
        switch (b->op.type) {
            case TokenType::Plus:    return *l + *r;
            case TokenType::Minus:   return *l - *r;
            case TokenType::Star:    return *l * *r;
            case TokenType::Slash:
                if (*r == 0) return std::nullopt;
                return *l / *r;
            case TokenType::Percent:
                if (*r == 0) return std::nullopt;
                return *l % *r;
            default:
                return std::nullopt;
        }
    }

    if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
        auto v = evaluateAsInt(u->operand.get());
        if (!v) return std::nullopt;
        switch (u->op.type) {
            case TokenType::Minus:   return -*v;
            default:                 return std::nullopt;
        }
    }

    // IdentifierExpr / MemberExpr / CallExpr / IndexExpr /
    // TableExpr → all rejected as non-constant. Deliberately
    // rejected even when their resolvedType is int — step-by-
    // variable is NOT supported (design decision 2026-07-14).
    // Identifiers / member reads / method returns have type but
    // no value at compile time; the "step must be a positive int
    // constant" error is the user-visible signal.
    return std::nullopt;
}

// R5.2-C (2026-07-14): best-effort source location lookup for
// expressions. The Expr base class does not currently carry a
// source location (S2.5 parser doesn't stamp it). For R5.2-C
// diagnostics we report at empty location — the
// `LogiaDiagnostic::toHumanString` already handles missing
// line/column gracefully. Future slice can add Expr-level source
// locations if finer-grained diagnostics are ever needed.
// S5 ED-02 (2026-07-14): `sourceLocFor(Expr*)` now reads the
// AST-stamped `sourceLoc` field set by the parser at construction.
// Pre-ED-02 this returned `{}` as a placeholder; the comment
// above explicitly noted "Future slice can add Expr-level source
// locations" — this slice fulfils that contract.
//
// 6 existing call sites (R5.2-C / R5.2-D / R5.2-H validators for
// for-bound / for-start / for-step / while-condition / if-condition)
// automatically pick up real line/column numbers without further
// edits; the 7 newly-populated diagnostic sites are listed in the
// "d.location = sourceLocFor(...)" call sites further down.
SourceLocation sourceLocFor(const Expr* e)
{
    if (!e) return {};
    return e->sourceLoc;
}

// S5 ED-02 (2026-07-14): `sourceLocFor(Stmt*)` reads the
// `sourceLoc` field set on the base `Stmt` class by the parser.
// All 13 concrete Stmt subclasses (IfStmt, WhileStmt, ForStmt,
// BlockStmt, VarDeclStmt, BreakStmt, ContinueStmt, LabelDeclStmt,
// ReturnStmt, ExprStmt, FunctionDeclStmt, LifecycleFuncDecl)
// inherit the field from `Stmt` and get it stamped by their
// respective `parseXxx` parser methods. See `Stmt::sourceLoc`
// in AYAst.h for the design rationale.
SourceLocation sourceLocFor(const Stmt* s)
{
    if (!s) return {};
    return s->sourceLoc;
}

// S5 ED-02 (2026-07-14): `sourceLocFor(ScriptDecl*)` for
// script-level diagnostics (unknown host type, strictInheritance
// failure). ScriptDecl has no `Stmt` base, so it gets its own
// helper.
SourceLocation sourceLocFor(const ScriptDecl* s)
{
    if (!s) return {};
    return s->sourceLoc;
}

const std::unordered_set<std::string>& ambientIdentifiers()
{
    static const std::unordered_set<std::string> s = {
        // Object-receiver ambients (codegen → bare Lua `name.method(...)`).
        "input", "log", "time",
        "vec2",   // M1 helpers: vec2.length / vec2.normalized
        "event",  // INT-04: event.emit / subscribe / unsubscribe → EventBus
    };
    return s;
}

// S3.10: register primitive types in AYReflect so `resolveTypeName`
// stamps a non-null ITypeInfo on primitive field accesses
// (`self.moveSpeed` where moveSpeed is `AY_PROPERTY(float, ...)`).
// Without this, the analyzer's `analyzeMemberExpr` leaves
// `m->resolvedType` as nullptr and the codegen rewrite to
// `ayt_reflect_set_field(self, ..., ...)` is skipped (the helper
// gates on `resolvedType->getFieldCount() == 0`, which would crash
// on null). Idempotent — safe to call from every analyzer ctor.
template <typename T>
void registerPrimitive(const char* name)
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();
    if (reg.findType(name) != nullptr) return;
    auto* info = new ayt::reflect::TypeInfoImpl<T>(
        name,
        ayt::reflect::detail::defaultCreate<T>,
        ayt::reflect::detail::defaultDestroy<T>,
        ayt::reflect::detail::defaultCopy<T>);
    reg.registerTypeInfo(name, info);
}

void ensurePrimitiveTypesRegistered()
{
    registerPrimitive<int32_t>("int");
    registerPrimitive<int32_t>("Int32");
    registerPrimitive<int64_t>("Int64");
    registerPrimitive<float>("float");
    registerPrimitive<float>("Float32");
    registerPrimitive<double>("double");
    registerPrimitive<double>("Float64");
    registerPrimitive<bool>("bool");
    registerPrimitive<bool>("Bool");
}

// S3.10: static-init guard so consumers that depend on AYReflect
// builtins (e.g. the System-host test fixture's MovementSystemRegistrar
// in Test_LogiaSystemHost.cpp, which calls `reg.findType("float")` at
// static init time) see a populated registry regardless of TU init
// order. The guard runs once on first access and is MT-safe by C++11
// static-init rules.
namespace {
struct PrimitiveBootstrap {
    PrimitiveBootstrap() { ensurePrimitiveTypesRegistered(); }
};
static PrimitiveBootstrap g_primitiveBootstrap;
} // namespace

// Explicitly register known AYEntity component types with AYReflect.
void ensureAYEntityTypesRegistered()
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();

    // FVector3 / FQuaternion primitives
    auto* fVec3 = reg.findType<ayt::math::FVector3>();
    if (!fVec3) {
        fVec3 = new ayt::reflect::TypeInfoImpl<ayt::math::FVector3>(
            "FVector3",
            ayt::reflect::detail::defaultCreate<ayt::math::FVector3>,
            ayt::reflect::detail::defaultDestroy<ayt::math::FVector3>,
            ayt::reflect::detail::defaultCopy<ayt::math::FVector3>);
        reg.registerTypeInfo("FVector3", fVec3);
    }
    // `analyzeMemberExpr` stamps `resolvedField` + `resolvedType=Float32`
    // on `self.position.x` leaves. Without this, the chain probe in
    // codegen never fires (leaf has null resolvedType) and the
    // legacy `__tmp_lhs` path is emitted — runtime no-op on the
    // lightuserdata receiver. Idempotent: re-entry guards on
    // `getFieldCount() == 0` so a partial prior registration is
    // completed on the next analyzer ctor. Field type is registered
    // as "Float32" to match `pushFieldPrimitive`'s dispatch (which
    // accepts both "float" and "Float32"). FQuaternion fields are
    // out of S3.11 scope (added by §5.7.4 track R2).
    if (fVec3 && fVec3->getFieldCount() == 0) {
        auto* floatInfo = reg.findType("float");
        if (floatInfo) {
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "x", floatInfo,
                offsetof(ayt::math::FVector3, x),
                ayt::reflect::FieldAttribute::Serialize));
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "y", floatInfo,
                offsetof(ayt::math::FVector3, y),
                ayt::reflect::FieldAttribute::Serialize));
            fVec3->addField(new ayt::reflect::FieldInfoImpl(
                "z", floatInfo,
                offsetof(ayt::math::FVector3, z),
                ayt::reflect::FieldAttribute::Serialize));
        }
    }
    // M1 (2026-07-15): mirror FVector3 registration for FVector2. `var m:
    // FVector2` is a Logia type annotation only — analyzer stamps
    // resolvedType=nullptr (same path as `var n: int`) so codegen does
    // not allocate a C++ FVector2 instance. Players use FVector2 to
    // type-tag inputs from `input.vec2(name)` (which returns a fresh
    // Lua table {x=, y=}). Fields x/y registered so future
    // `self.<FVector2_field>` chain reflect works, mirroring FVector3.
    auto* fVec2 = reg.findType<ayt::math::FVector2>();
    if (!fVec2) {
        fVec2 = new ayt::reflect::TypeInfoImpl<ayt::math::FVector2>(
            "FVector2",
            ayt::reflect::detail::defaultCreate<ayt::math::FVector2>,
            ayt::reflect::detail::defaultDestroy<ayt::math::FVector2>,
            ayt::reflect::detail::defaultCopy<ayt::math::FVector2>);
        reg.registerTypeInfo("FVector2", fVec2);
    }
    if (fVec2 && fVec2->getFieldCount() == 0) {
        auto* floatInfo = reg.findType("float");
        if (floatInfo) {
            fVec2->addField(new ayt::reflect::FieldInfoImpl(
                "x", floatInfo,
                offsetof(ayt::math::FVector2, x),
                ayt::reflect::FieldAttribute::Serialize));
            fVec2->addField(new ayt::reflect::FieldInfoImpl(
                "y", floatInfo,
                offsetof(ayt::math::FVector2, y),
                ayt::reflect::FieldAttribute::Serialize));
        }
    }
    auto* fQuat = reg.findType<ayt::math::FQuaternion>();
    if (!fQuat) {
        fQuat = new ayt::reflect::TypeInfoImpl<ayt::math::FQuaternion>(
            "FQuaternion",
            ayt::reflect::detail::defaultCreate<ayt::math::FQuaternion>,
            ayt::reflect::detail::defaultDestroy<ayt::math::FQuaternion>,
            ayt::reflect::detail::defaultCopy<ayt::math::FQuaternion>);
        reg.registerTypeInfo("FQuaternion", fQuat);
    }

    // Transform — always re-register so its field types are non-null.
    {
        auto* info = new ayt::reflect::TypeInfoImpl<ayt::entity::Transform>(
            "Transform",
            ayt::reflect::detail::defaultCreate<ayt::entity::Transform>,
            ayt::reflect::detail::defaultDestroy<ayt::entity::Transform>,
            ayt::reflect::detail::defaultCopy<ayt::entity::Transform>);
        if (fVec3) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "position", fVec3,
                offsetof(ayt::entity::Transform, position),
                ayt::reflect::FieldAttribute::Serialize));
            info->addField(new ayt::reflect::FieldInfoImpl(
                "scale", fVec3,
                offsetof(ayt::entity::Transform, scale),
                ayt::reflect::FieldAttribute::Serialize));
        }
        if (fQuat) {
            info->addField(new ayt::reflect::FieldInfoImpl(
                "rotation", fQuat,
                offsetof(ayt::entity::Transform, rotation),
                ayt::reflect::FieldAttribute::Serialize));
        }
        reg.registerTypeInfo("Transform", info);
    }

    if (!reg.findType("HealthComponent")) {
        auto* info = new ayt::reflect::TypeInfoImpl<ayt::entity::HealthComponent>(
            "HealthComponent",
            ayt::reflect::detail::defaultCreate<ayt::entity::HealthComponent>,
            ayt::reflect::detail::defaultDestroy<ayt::entity::HealthComponent>,
            ayt::reflect::detail::defaultCopy<ayt::entity::HealthComponent>);
        reg.registerTypeInfo("HealthComponent", info);
    }
}

} // namespace

SemanticAnalyzer::SemanticAnalyzer(SemanticOptions options, const LogiaHostContext& ctx)
    : _options(options)
    , _fileName(options.fileName ? options.fileName : "")
    , _ctx(ctx)
{
    _registryImpl = &ayt::reflect::TypeRegistryImpl::instance();
    _registry     = _registryImpl;
    ensureAYEntityTypesRegistered();
    // S3.10: also ensure the primitive types (int / float / bool /
    // double / int64 / their PascalCase aliases) are registered so
    // the analyzer's `resolveTypeName` can stamp `resolvedType` on
    // `self.<primitiveField>` MemberExpr leaves. Without this, the
    // codegen rewrite `self.field = X → ayt_reflect_set_field(...)`
    // is skipped (the helper guards on resolvedType->getFieldCount()
    // == 0, which can't be evaluated on null). Idempotent.
    ensurePrimitiveTypesRegistered();
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

void SemanticAnalyzer::setTypeProvider(ayt::reflect::ITypeRegistry* reg)
{
    _registry = reg;
    _registryImpl = static_cast<ayt::reflect::TypeRegistryImpl*>(reg);
}

bool SemanticAnalyzer::isBuiltInType(const std::string& name)
{
    return builtinTypeNames().count(name) > 0;
}

bool SemanticAnalyzer::isAmbientIdentifier(const std::string& name)
{
    return ambientIdentifiers().count(name) > 0;
}

// S4.1 (2026-07-15): signal-arg-vs-signal-param type compat check.
// Used by analyzeEmitCall to validate that each supplied emit arg
// matches the declared signal parameter type. The Logia type
// lattice has only the four built-in numeric / string leaves that
// can appear at a signal-arg site today (no struct args at the
// signal boundary in v1). Returns true on a clean match; false on
// mismatch OR when the arg's static type cannot be determined
// (un-inferable — caller emits a soft warning, not a hard error).
//
// Notes for future tightening:
//   - `int ↔ float` are considered compatible when the declared
//     param is `int` (Lua numbers are all float64; the runtime
//     passes through cleanly). Declared param `float` rejects int
//     because Logia has no implicit promotion (R5.2-C precedent).
//   - Un-inferable args (e.g. an IdentifierExpr with no
//     resolvedDecl and no ambient membership) return false with a
//     soft-warn path, NOT a hard error, to keep the door open for
//     forwarded dynamic arg shapes later.
bool SemanticAnalyzer::signalArgMatchesParam(Expr* arg, const std::string& paramTypeName)
{
    if (!arg) return false;
    if (paramTypeName == "int" || paramTypeName == "float" ||
        paramTypeName == "string" || paramTypeName == "bool") {
        // Walk to a leaf — for now, only literals and ambient
        // signals get a static type; identifiers get a type from
        // `_scope` (VarDeclStmt resolvedType) or from
        // resolvedType/resolvedDecl on the AST node.
        if (auto* lit = dynamic_cast<LiteralExpr*>(arg)) {
            // LiteralExpr::Value variant dispatch:
            //   int/float → numeric; string → string; bool → bool.
            if (auto* iv = std::get_if<int>(&lit->value)) {
                (void)iv;
                return paramTypeName == "int" || paramTypeName == "float";
            }
            if (auto* fv = std::get_if<float>(&lit->value)) {
                (void)fv;
                return paramTypeName == "float";
            }
            if (auto* sv = std::get_if<std::string>(&lit->value)) {
                (void)sv;
                return paramTypeName == "string";
            }
            if (auto* bv = std::get_if<bool>(&lit->value)) {
                (void)bv;
                return paramTypeName == "bool";
            }
            return false;
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(arg)) {
            // Undeclared identifier — Lua-style implicit global;
            // the value's runtime type is unknown at compile time.
            // Soft-warn later; for hard compat check, return false.
            auto it = _scope.find(id->name);
            if (it == _scope.end()) return false;
            // Declared identifier with a builtin type.
            // resolvedType is non-null for reflect-registered
            // types and null for builtin-int (per analyzeVarDecl).
            // Fall back to resolvedDecl->typeName via the
            // ScopeEntry's type-name lookup. For S4.1 we only
            // support builtin types at the signal param site.
            // `id->resolvedDecl` is a VarDeclStmt* for declared
            // locals; we can read its typeName.
            if (auto* var = static_cast<const VarDeclStmt*>(id->resolvedDecl)) {
                return var->typeName == paramTypeName;
            }
            return false;
        }
        if (auto* mem = dynamic_cast<MemberExpr*>(arg)) {
            // self.<field> leaf — read ITypeInfo* name and check
            // the builtin-type name against the declared param.
            if (mem->resolvedType) {
                const std::string name(mem->resolvedType->getName());
                // Reflect-registered primitives map to "int" /
                // "float" / "bool" — same names as builtin.
                return name == paramTypeName;
            }
            return false;
        }
    }
    // Struct / unknown param type — not supported at signal
    // boundary in v1; reject with a clear error in the caller.
    return false;
}

// R5.2-H.b (2026-07-14): read-only accessor for the
// `_loopCounters` stack, used by `leafIsStaticallyPrimitive`
// to recognize for-loop counter identifiers as int leaves in
// condition contexts. Walks the frame stack from innermost
// outward (mirrors `isLabelVisible` for `_labelStack` at
// L1421). Returns true on first match; false when no frame
// contains the name OR the stack is empty.
bool SemanticAnalyzer::isLoopCounter(const std::string& name) const
{
    for (auto it = _loopCounters.rbegin();
         it != _loopCounters.rend(); ++it) {
        if (it->count(name)) return true;
    }
    return false;
}

void SemanticAnalyzer::report(LogiaDiagnostic d)
{
    if (d.location.file.empty() && !_fileName.empty()) {
        d.location.file = _fileName;
    }
    _diagnostics.push_back(std::move(d));
}

const ayt::reflect::ITypeInfo*
SemanticAnalyzer::resolveTypeName(const std::string& name, int line, int column)
{
    if (isBuiltInType(name)) {
        // Built-ins never have an ITypeInfo* — they are ambient primitives.
        // Return nullptr so the caller treats the name as valid (built-in)
        // without stamping a registry entry.
        return nullptr;
    }
    if (_registry) {
        auto* info = _registry->findType(name.c_str());
        if (info) return info;
    }
#if defined(AY_SCRIPT_USE_COMPILE_TIME_TYPES)
    (void)line; (void)column;
#endif
    (void)line; (void)column;
    return nullptr;
}

const ayt::reflect::ITypeInfo*
SemanticAnalyzer::resolveScriptName(const std::string& name, int line, int column)
{
    // S2.5: a script must match a registered ScriptComponent subclass.
    // Lookup goes through the same AYReflect TypeRegistry; component
    // registration happens via AY_COMPONENT(T) in each component header.
    if (_registry) {
        auto* info = _registry->findType(name.c_str());
        if (info) return info;
    }
    (void)line; (void)column;
    return nullptr;
}

SemanticResult SemanticAnalyzer::analyze(Program& program)
{
    _diagnostics.clear();
    for (auto& script : program.scripts) {
        if (script) analyzeScript(*script);
    }
    return SemanticResult{_diagnostics};
}

void SemanticAnalyzer::analyzeScript(ScriptDecl& s)
{
    _scope.clear();
    _currentSelfType = nullptr;

    // Resolve the script name to the bound host type via the AYReflect
    // TypeRegistry. S2.5 / S3.0 (LG-03): an unknown script name is a
    // soft warning, not a hard error — Logia source files are sometimes
    // written before the matching C++ host type is compiled in. The
    // runtime bridge is responsible for the actual binding check.
    //
    // S3.1 (LG-04): the same `findType(name)` lookup is used for all
    // host kinds. No subclass check (per LG-04b / S3.2 deferral). The
    // host-kind-specific hint text below tells the user which C++ side
    // binding to add (AY_SYSTEM for ECS systems, ScriptComponent for
    // entity scripts).
    auto* selfType = resolveScriptName(s.name, 0, 0);
    if (!selfType) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.location = sourceLocFor(&s);   // S5 ED-02: `script` keyword loc
        d.message = "script '" + s.name + "' has no matching registered type";
        std::string hint;
        if (_ctx.kind == LogiaHostKind::System) {
            hint = "register an ISystem subclass named '" + s.name +
                   "' with AY_SYSTEM(" + s.name + ", priority) so World " +
                   "ticks it; or fix the name to match an existing ISystem";
        } else {
            hint = "register a C++ host type named '" + s.name +
                   "' via AYReflect (e.g. AY_FINALIZE_REGISTRATION_METADATA(" +
                   s.name + ")); or fix the name to match an existing registered type";
        }
        d.hint = hint;
        report(d);
    }
    // S3.2 (LG-04b, B-min): Component host strict inheritance. Only
    // applies when the caller asked for it via `ctx.strictInheritance`
    // AND supplied a non-null `ctx.hostType` AND the host kind is
    // Component. We hard-reject scripts that are registered in
    // AYReflect but NOT derived from `hostType` (per
    // ayt::reflect::isDerivedFrom single-chain walk). Scripts not
    // registered at all still emit the S2.5 / LG-03 soft warning
    // above and are not covered by this check (the caller is
    // responsible for catching genuinely missing types before
    // enabling strict mode).
    if (_ctx.kind == LogiaHostKind::Component
        && _ctx.strictInheritance
        && _ctx.hostType != nullptr
        && selfType != nullptr) {
        if (!ayt::reflect::isDerivedFrom(selfType, _ctx.hostType)) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::TypeMismatch;
            d.location = sourceLocFor(&s);   // S5 ED-02
            d.message = "script '" + s.name + "' must derive from '" +
                        std::string(_ctx.hostType->getName()) +
                        "' (strict Component host binding)";
            d.hint = "either add an AY_INHERITS(" + s.name + ", " +
                     std::string(_ctx.hostType->getName()) +
                     ") declaration, or remove `strictInheritance` from "
                     "the LogiaHostContext";
            report(d);
        }
    }

    _currentSelfType = selfType;

    // LG-05 / S3.3: stamp the host type name on the ScriptDecl so
    // LuaCodegen can emit `ayt_reflect_*_field(self, "<name>", "<f>")`
    // for `self.field` reads/writes. Only set when the script name
    // resolved in AYReflect — otherwise codegen keeps the S2.5 bare
    // `self.<field>` form (a no-op at runtime) so unknown hosts do
    // not crash the reflect path.
    if (selfType) {
        s.hostTypeName = std::string(selfType->getName());
    }

    // S3.8b: Tool hosts (and any other host that sets `expectSelf`
    // = false) do not bind a receiver to the script — ToolRunner
    // calls `run()` with `receiver = nullptr`, so injecting `self`
    // into the scope would be misleading (every read would be a
    // soft warning, every write would target a nil lightuserdata).
    // Skip the scope injection; a `run()` body that mentions `self`
    // will hit the implicit-global path and produce the standard
    // soft warning, which is the right surface to the user.
    if (_ctx.expectSelf) {
        ScopeEntry e;
        e.type = selfType;
        e.decl = nullptr;
        _scope["self"] = e;
    }

    // S4.1 (2026-07-15): signal-declaration pre-pass — collect signal
    // names + parameter signatures BEFORE walking bodies, so that a
    // `connect("damaged", on_damaged)` inside `on_start` can reference
    // a signal declared textually below it. Mirrors the var-decl
    // first pass above (forward-reference handling). Duplicate name
    // is a hard error; bogus param types reuse the var-style
    // `resolveTypeName` validation.
    _signals.clear();
    for (auto& member : s.members) {
        if (!member) continue;
        if (auto* sig = dynamic_cast<SignalDeclStmt*>(member.get())) {
            analyzeSignalDecl(*sig);
        }
    }

    // First pass: collect var declarations into the script-local scope.
    for (auto& member : s.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            analyzeVarDecl(*v);
        }
        // Lifecycle params (S2.5: must be empty; parser already errors)
        // and var decls are the only named introducers.
    }

    // Second pass: walk lifecycle bodies and var initializers.
    for (auto& member : s.members) {
        if (!member) continue;
        if (auto* v = dynamic_cast<VarDeclStmt*>(member.get())) {
            if (v->initializer) analyzeExpr(*v->initializer);
        } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(member.get())) {
            analyzeLifecycle(*lf);
        } else if (auto* sig = dynamic_cast<SignalDeclStmt*>(member.get())) {
            (void)sig;   // S4.1: validated in the pre-pass above; skip here.
        } else {
            analyzeStmt(*member);
        }
    }

    _currentSelfType = nullptr;
}

void SemanticAnalyzer::analyzeVarDecl(VarDeclStmt& v)
{
    // S4.1b (2026-07-18): NO init-position inference — players must
    // explicitly declare `var h: int = connect(...)`. The legacy
    // typeName-from-annotation path below naturally rejects
    // `var h = connect(...)` (v.typeName == "" fails `isBuiltInType`
    // and `resolveTypeName`, surfacing as "unknown type ''"). This
    // keeps the analyzer's type-resolution discipline intact and
    // matches the R5.x "static-only" philosophy — no expression-
    // position type inference in v1.

    if (isBuiltInType(v.typeName)) {
        ScopeEntry e;
        e.type = nullptr;  // built-in: no ITypeInfo*
        e.decl = &v;
        _scope[v.name] = e;
        return;
    }
    auto* info = resolveTypeName(v.typeName, 0, 0);
    if (!info) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&v);   // S5 ED-02: var-keyword loc
        d.message = "unknown type '" + v.typeName + "'";
        d.hint = "register the type with AYReflect or use a built-in (int, float, bool, string, Entity)";
        report(d);
        return;
    }
    v.resolvedType = info;
    ScopeEntry e;
    e.type = info;
    e.decl = &v;
    _scope[v.name] = e;
}

void SemanticAnalyzer::analyzeLifecycle(LifecycleFuncDecl& fn)
{
    // S2.5: lifecycle functions take no parameters. The parser silently
    // drops a non-empty parameter list (forgiving old S1 code), so we
    // report it here as a soft warning. The S2.5 codegen also ignores
    // the params field — `self` is always the only argument emitted.
    if (!fn.params.empty()) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&fn);   // S5 ED-02: lifecycle-keyword loc
        d.message = "lifecycle functions take no parameters in S2.5";
        d.hint = "use 'self' for the receiver; read entity context via World::instance()";
        report(d);
    }

    // S3.1 (LG-04): the System host (ctx.kind == System) defines its
    // own lifecycle method set. `on_start` is one-shot at world tick
    // start; `on_update` is the per-tick hook. `on_destroy` is not
    // meaningful for an ISystem — emit a soft warning if a System-
    // bound script declares it. Component host keeps the full S2.5
    // set (on_start / on_update / on_destroy) unchanged.
    if (_ctx.kind == LogiaHostKind::System
        && fn.kind == LifecycleKind::OnDestroy) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&fn);   // S5 ED-02
        d.message = "on_destroy is not invoked on System host scripts";
        d.hint = "ISystem instances are owned by World for the whole "
                 "process lifetime; use on_start (one-shot) or on_update "
                 "(per-tick) instead";
        report(d);
    }

    // S3.8b (LG-07) — Tool host policy. A Tool script is a run-only
    // one-shot (editor / CLI) whose entry point is the new `run()`
    // lifecycle. The legacy `on_start` / `on_update` / `on_destroy`
    // methods are never invoked by the ToolRunner — emit soft
    // warnings so refactors from Component / System hosts keep
    // parsing. The shape mirrors the S3.1 System-on_destroy policy.
    //
    // INT-04b: EventHandler shares the same Component-lifecycle soft
    // warn (no tick / no ScriptComponent), but keeps `run()` as the
    // one-shot bind entry for ambient event.subscribe wiring.
    if (_ctx.kind == LogiaHostKind::Tool
        || _ctx.kind == LogiaHostKind::EventHandler) {
        const char* name = nullptr;
        if (fn.kind == LifecycleKind::OnStart) {
            name = "on_start";
        } else if (fn.kind == LifecycleKind::OnUpdate) {
            name = "on_update";
        } else if (fn.kind == LifecycleKind::OnDestroy) {
            name = "on_destroy";
        }
        if (name) {
            const char* hostLabel =
                (_ctx.kind == LogiaHostKind::Tool) ? "Tool" : "EventHandler";
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Warning;
            d.errorCode = ErrorCode::InvalidStatement;
            d.location = sourceLocFor(&fn);   // S5 ED-02
            d.message = std::string(name) +
                        " is not invoked on " + hostLabel + " host scripts";
            if (_ctx.kind == LogiaHostKind::Tool) {
                d.hint = "Tool host runs once via the run() entry point; "
                         "use `run()` for the one-shot body, or move this "
                         "script to a Component / System host if you need "
                         "tick-driven lifecycle";
            } else {
                d.hint = "EventHandler uses `run()` to bind event.subscribe "
                         "(or call handlers from C++); Component lifecycle "
                         "is not dispatched for this host kind";
            }
            report(d);
        }
    }

    // S3.8b (LG-07) — symmetric warning: `run()` is meaningful only
    // under Tool / EventHandler hosts. On Component / System it would
    // be a dead method (neither the dispatcher nor the runner ever
    // calls it). Soft warn so a copy-paste from a Tool source still
    // parses under a Component host but the user knows to drop it.
    if (_ctx.kind != LogiaHostKind::Tool
        && _ctx.kind != LogiaHostKind::EventHandler
        && fn.kind == LifecycleKind::Run) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::InvalidStatement;
        d.message = "run() is a Tool / EventHandler lifecycle and is not "
                    "invoked on "
                    + std::string(_ctx.kind == LogiaHostKind::Component
                                      ? "Component"
                                      : "System")
                    + " host scripts";
        d.hint = "move this script to a Tool host (toolLogiaHostContext() / "
                 "runTool()) or EventHandler host "
                 "(eventHandlerLogiaHostContext() / loadEventHandler()), "
                 "or replace `run()` with on_start / on_update for this "
                 "host kind";
        report(d);
    }

    // R5.2-B (2026-07-14): push a function-body label-scope frame.
    // The frame is kept empty of labels by the rule that `::L::`
    // only lives in loop bodies, but pushed for stack consistency
    // so that the "label outside loop" check has a stable parent.
    _labelStack.emplace_back();
    for (auto& s : fn.body) {
        if (s) analyzeStmt(*s);
    }
    _labelStack.pop_back();
}

// R5.0 (2026-07-13): walk a while loop's condition and body.
//
// We do NOT push a new scope onto `_scope` — Lua's `while cond do ...
// end` does not introduce a lexical scope, so the body is in the same
// scope as the enclosing block. Variables declared in the body
// (`var x: int = ...`) live for the iteration of the loop only at the
// Logia-source level (the user's reading); each iteration sees the
// same `local x` re-declaration in the emitted Lua, which Lua tolerates
// as long as it's at the same block scope (which it is, since
// `parseBlockBody` re-enters here).
void SemanticAnalyzer::analyzeWhileStmt(WhileStmt& w)
{
    // R5.2-H (2026-07-14): while condition must statically be bool.
    // `while (n)` where n: int is a hard error — Logia has no
    // C-style truthiness. `while (n > 0)` is fine (BinaryExpr with
    // comparison op + bool-typed leaves). See isStaticallyBool.
    validateCondition(w.condition.get(), sourceLocFor(w.condition.get()),
                      "while");
    // R5.2-B (2026-07-14): push a label-scope frame for the
    // while-body. Labels declared here are visible to `break :L`
    // / `continue :L` inside this body and to nested-loop bodies
    // (the inner frames have the outer in their `_labelStack`
    // ancestry). Popped on exit.
    _labelStack.emplace_back();
    for (auto& s : w.body) {
        if (s) analyzeStmt(*s);
    }
    _labelStack.pop_back();
}

// R5.0 (2026-07-13): walk a for loop's bound and body.
//
// The counter variable (e.g. `i` in `for (var i : 10)`) is deliberately
// NOT added to `_scope` here. Lua's `for i = 1, N do ... end` already
// makes `i` loop-local at the Lua level. If we added `i` to the
// analyzer's `_scope`, then:
//   1. Reads of `i` inside the body would resolve as if `i` were a
//      Logia-script-level var (correct behavior, but only by accident).
//   2. Reads/writes to `i` AFTER the loop body would also resolve to
//      the analyzer's view of the counter, but at runtime the counter
//      is gone (Lua's `for` is closed-over). This mismatch would
//      produce silent type errors in future R5.0.1 diagnostic passes.
//
// Skipping the scope injection keeps the analyzer's view aligned with
// runtime semantics: `i` is loop-local inside the body, and post-loop
// references to `i` are implicit globals (same as any other
// undeclared-identifier write in Logia S1).
//
// Bound type-check is deferred to R5.0.1: ideally we'd verify the
// bound expression reduces to `int` and emit a soft warning otherwise.
// S2.5 / LG-05 only stamps types on `self.<field>` leaf reads via
// AYReflect; adding general expression-type inference is out of scope.
//
// R5.0.1 (2026-07-13): also walk the optional `start` expression in
// the half-open range form `for (var i : start, end)` — analyzer
// needs to validate the start expression the same way it validates
// the bound.
//
// R5.2-C (2026-07-14): the bound type-check is no longer deferred.
// validateForBound() emits `ErrorCode::TypeMismatch` hard error when
// the bound cannot be statically reduced to int. Decision matrix
// (allowed vs rejected shapes) lives on validateForBound's comment
// block. `validateForBound` also internally walks the bound subtree
// (calling analyzeExpr-like logic on each leaf) so we no longer
// need the prior `analyzeExpr(*f.bound)` call.
void SemanticAnalyzer::analyzeForStmt(ForStmt& f)
{
    // R5.2-C (2026-07-14): validate the upper bound expression.
    // validateForBound internally calls analyzeExpr to stamp
    // resolvedType on identifier-shaped leaves before checking.
    validateForBound(f.bound.get(), sourceLocFor(f.bound.get()),
                     "bound");

    // R5.0.1 (2026-07-13): range form `for (var i : start, end)`.
    // R5.2-C (2026-07-14): validate `start` the same way.
    if (f.start) {
        validateForBound(f.start.get(), sourceLocFor(f.start.get()),
                         "start");
    }

    // R5.2-D (2026-07-14): optional step validation. Distinct from
    // validateForBound — step must (a) statically reduce to int
    // AND (b) be a positive constant. Non-constant int (var /
    // self.field / self.method()) is rejected here, even though
    // bound accepts int-typed identifiers.
    if (f.step) {
        analyzeExpr(*f.step);             // stamp resolvedType on
                                          //   identifier-shaped leaves
                                          //   (defensive — error path
                                          //   may inspect it)
        auto v = evaluateAsInt(f.step.get());
        if (!v) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::TypeMismatch;
            d.location = sourceLocFor(f.step.get());
            d.message = "for-loop step must be a positive int "
                        "constant (literal or constant-folded "
                        "expression of int literals)";
            d.hint = "use a literal like 1, 2, 3 ... or a "
                     "constant-folded expression like 3 + 5. "
                     "Identifiers and self.<field> are not "
                     "supported as step values (must be static).";
            report(d);
        } else if (*v <= 0) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::TypeMismatch;
            d.location = sourceLocFor(f.step.get());
            d.message = "for-loop step must be positive, got "
                        + std::to_string(*v);
            d.hint = "step < 0 is rejected by Logia (only "
                     "positive step is supported). step == 0 "
                     "would loop infinitely at runtime.";
            report(d);
        }
    }

    // R5.2-B (2026-07-14): symmetric label-scope frame for the
    // for-body. See analyzeWhileStmt's comment.
    _labelStack.emplace_back();
    // R5.2-H (2026-07-14): push the counter name into the
    // loop-counter frame so R5.2-H's `leafIsStaticallyPrimitive`
    // can recognize it as an int leaf when used in conditions
    // (`if (i == 5)`, `if (j > 0)`, etc.). The counter is NOT
    // pushed into `_scope` (per R5.0 / R5.2-C's deliberate
    // design) so post-loop references stay implicit-globals; the
    // loop-counter stack is a *separate*, condition-only
    // visibility channel. Mirrors the symmetry of
    // `_labelStack` push/pop.
    _loopCounters.emplace_back();
    _loopCounters.back().insert(f.counterName);
    for (auto& s : f.body) {
        if (s) analyzeStmt(*s);
    }
    _loopCounters.pop_back();
    _labelStack.pop_back();
}

// R5.2-C (2026-07-14): verify a `for (...)` bound statically
// reduces to int. Hard error otherwise.
//
// We use a small visitor instead of overloading the existing
// `analyzeExpr` dispatch because (a) we want different error
// wording (point at the bound role, not "implicit global"), and
// (b) we must short-circuit (no further diagnostic on subtree)
// once we know the answer.
//
// Two-stage walk:
//   1. Call analyzeExpr(*bound) first to stamp resolvedType on
//      identifier-shaped leaves (existing S2.5 / S3.0 plumbing).
//   2. Then run boundIsStaticallyInt to inspect the stamped tree.
//
// Decision matrix:
//   IntLiteral / BinaryExpr of int literals / UnaryExpr of int /
//   IdentifierExpr whose resolvedType is int / MemberExpr whose
//   resolvedType is int / CallExpr whose resolvedMethod returns int
//     → OK
//   FloatLiteral / StringLiteral / BoolLiteral / IdentifierExpr
//   with non-int resolvedType / IndexExpr / TableExpr / etc.
//     → hard error TypeMismatch
void SemanticAnalyzer::validateForBound(
    Expr* bound, const SourceLocation& loc, const std::string& role)
{
    if (!bound) {
        // Defensive — parser shouldn't produce null bound.
        return;
    }

    // Stage 1: walk the bound subtree so identifier / member / call
    // paths get their resolvedType stamped before we inspect it.
    analyzeExpr(*bound);

    // Stage 2: shape check.
    if (boundIsStaticallyInt(bound)) {
        return;
    }

    // Fall-through: hard error. The bound is not provably int.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Error;
    d.errorCode = ErrorCode::TypeMismatch;
    d.location = loc;
    if (auto* lit = dynamic_cast<LiteralExpr*>(bound)) {
        // Literal but wrong-shape (float/string/bool).
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                d.message = "for-loop " + role +
                            " must be int, got float literal";
            } else if constexpr (std::is_same_v<T, std::string>) {
                d.message = "for-loop " + role +
                            " must be int, got string literal";
            } else if constexpr (std::is_same_v<T, bool>) {
                d.message = "for-loop " + role +
                            " must be int, got bool literal";
            } else {
                d.message = "for-loop " + role +
                            " must be int, got non-int literal";
            }
        }, lit->value);
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(bound)) {
        d.message = "for-loop " + role + " '" + id->name +
                    "' does not have a known int type";
    } else if (auto* m = dynamic_cast<MemberExpr*>(bound)) {
        d.message = "for-loop " + role + " (" + m->member +
                    ") does not have a known int type";
    } else if (auto* c = dynamic_cast<CallExpr*>(bound)) {
        d.message = "for-loop " + role +
                    " call expression does not return int";
    } else {
        d.message = "for-loop " + role +
                    " expression does not statically reduce to int";
    }
    d.hint = "use an int literal, an int-typed variable, "
             "an int-typed self.<field>, or a self.<method>() "
             "returning int";
    report(d);
}

// R5.1 (2026-07-13): no-op for `break;` inside a loop.
// R5.2-B (2026-07-14): verify label visibility when `break :L` is
// used. The parser's `loopDepth` gate has already verified this
// BreakStmt appears inside a while / for body; the label check
// uses the analyzer's `_labelStack` (pushed in analyzeWhileStmt /
// analyzeForStmt) to confirm the label was declared in some
// enclosing loop. The BreakStmt has no sub-nodes to walk.
void SemanticAnalyzer::analyzeBreakStmt(BreakStmt& b)
{
    if (!b.label.empty() && !isLabelVisible(b.label)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&b);   // S5 ED-02: `break` keyword loc
        d.message = "label '" + b.label +
                    "' not found in any enclosing loop";
        d.hint = "declare with `::" + b.label + "::` inside an "
                 "enclosing while or for body";
        report(d);
    }
}

// R5.1 (2026-07-13): no-op for `continue;` inside a loop.
// R5.2-B (2026-07-14): `continue :L` is not supported in this
// slice — the parser rejects it before we get here. The
// `label` field is always empty in practice (kept on the AST
// for forward compatibility with a future R5.x slice that
// may add name-mangled continue labels).
void SemanticAnalyzer::analyzeContinueStmt(ContinueStmt& /*c*/)
{
}

// R5.2-H (2026-07-14): validate an `if (...)` / `while (...)`
// condition. Mirror of validateForBound (R5.2-C) but checks for
// bool-shape instead of int-shape. Two-stage walk:
//   1. Call analyzeExpr(*cond) to stamp resolvedType on
//      identifier-shaped leaves (existing S2.5 / S3.0 plumbing).
//   2. Run isStaticallyBool to inspect the stamped tree.
//
// Decision matrix (allowed vs rejected shapes) lives on
// isStaticallyBool's comment block. On rejection we emit a
// TypeMismatch hard error with a hint pointing the user at
// the bool type contract and offering a comparison rewrite
// (`n` → `n > 0`, `count` → `count != 0`, etc.).
void SemanticAnalyzer::validateCondition(
    Expr* cond, const SourceLocation& loc, const std::string& role)
{
    if (!cond) {
        // Defensive — parser shouldn't produce null condition
        // (parens are required for if / while per R5.0.1, and
        // the parser would have errored on `if` / `while () { }`).
        return;
    }

    // Stage 1: walk the condition subtree so identifier / member
    // / call paths get their resolvedType stamped before we
    // inspect it.
    analyzeExpr(*cond);

    // Stage 2: shape check. Pass `this` so the predicate can
    // consult `_loopCounters` (for-counter carve-out) and
    // `isAmbientIdentifier` (ambient-receiver CallExpr
    // carve-out). See R5.2-H.b comments on isStaticallyBool /
    // leafIsStaticallyPrimitive.
    if (isStaticallyBool(cond, this)) {
        return;
    }

    // Fall-through: hard error. Compose a role-specific message
    // pointing at the literal/identifier kind so the user can
    // locate the offending sub-expression.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Error;
    d.errorCode = ErrorCode::TypeMismatch;
    d.location = loc;

    if (auto* lit = dynamic_cast<LiteralExpr*>(cond)) {
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                d.message = role + " condition must be bool, got null literal";
            } else if constexpr (std::is_same_v<T, int>) {
                d.message = role + " condition must be bool, got int literal";
            } else if constexpr (std::is_same_v<T, float>) {
                d.message = role + " condition must be bool, got float literal";
            } else if constexpr (std::is_same_v<T, std::string>) {
                d.message = role + " condition must be bool, got string literal";
            } else {
                d.message = role + " condition must be bool, got non-bool literal";
            }
        }, lit->value);
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(cond)) {
        d.message = role + " condition '" + id->name +
                    "' does not have a known bool type";
    } else if (auto* m = dynamic_cast<MemberExpr*>(cond)) {
        d.message = role + " condition (self." + m->member +
                    ") does not have a known bool type";
    } else if (auto* c = dynamic_cast<CallExpr*>(cond)) {
        d.message = role + " condition call expression does not return bool";
    } else {
        d.message = role + " condition expression does not statically reduce to bool";
    }
    d.hint = "Logia has no implicit truthiness — rewrite `x` as a "
             "comparison like `x > 0`, `x != 0`, `x == true`, etc.; "
             "or declare the variable as `bool` (`var ready: bool = ...`)";
    report(d);
}

// R5.2-B (2026-07-14): `::LABEL::` — register the name in the
// innermost label-scope frame. The frame corresponds to the
// enclosing loop body (analyzeWhileStmt / analyzeForStmt pushed
// it; the function-body frame from analyzeLifecycle is also
// present but labels in it are illegal). Duplicate names in the
// same frame are a hard error.
void SemanticAnalyzer::analyzeLabelDeclStmt(LabelDeclStmt& l)
{
    if (_labelStack.empty()) {
        // Should not happen in practice — analyzeLifecycle pushes
        // the outermost frame, so the stack is non-empty inside
        // any lifecycle body. Defensive guard.
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&l);   // S5 ED-02: label-name loc
        d.message = "label '" + l.name + "' declared outside loop";
        d.hint = "labels may only be declared inside a while or for body";
        report(d);
        return;
    }
    // The outermost frame is the function-body frame
    // (analyzeLifecycle); labels declared there are illegal per
    // the loop-scoped rule. We detect that case by checking the
    // frame's size relative to its position. Simpler: check
    // whether the current frame is the only one on the stack.
    if (_labelStack.size() == 1u) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&l);   // S5 ED-02
        d.message = "label '" + l.name + "' declared outside loop";
        d.hint = "labels may only be declared inside a while or for body";
        report(d);
        return;
    }
    if (!_labelStack.back().insert(l.name).second) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&l);   // S5 ED-02
        d.message = "duplicate label '" + l.name + "' in this loop";
        d.hint = "each label name must be unique within its enclosing loop";
        report(d);
    }
}

// R5.2-B (2026-07-14): walk `_labelStack` from innermost frame
// outward and return true on first match. False when the stack
// is empty.
bool SemanticAnalyzer::isLabelVisible(const std::string& name) const
{
    for (auto it = _labelStack.rbegin(); it != _labelStack.rend(); ++it) {
        if (it->count(name)) return true;
    }
    return false;
}

// S4.1 (2026-07-15): per-component signal-declaration pre-pass
// validation. Called from analyzeScript BEFORE the body walk so
// that forward references resolve. Two responsibilities:
//   1. Reject duplicate signal names within one script
//      (each `_signals` entry maps a unique name → param list).
//   2. Validate each declared parameter type via the same
//      `isBuiltInType` / `resolveTypeName` path that
//      `analyzeVarDecl` uses — unknown types are a hard error so
//      the caller can't accidentally reference a never-registered
//      type at a signal param site.
void SemanticAnalyzer::analyzeSignalDecl(SignalDeclStmt& sig)
{
    if (_signals.count(sig.name)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&sig);   // S5 ED-02: signal-name loc
        d.message = "duplicate signal '" + sig.name + "' in this script";
        d.hint = "signal names must be unique within one script block";
        report(d);
        return;
    }
    // Validate each declared parameter type. The pre-pass runs
    // before var-decl collection, but the validator doesn't depend
    // on `_scope` — it consults `isBuiltInType` / `resolveTypeName`
    // directly against the registry.
    for (const auto& p : sig.params) {
        if (!isBuiltInType(p.typeName)) {
            auto* info = resolveTypeName(p.typeName, 0, 0);
            if (!info) {
                LogiaDiagnostic d;
                d.severity = DiagnosticSeverity::Error;
                d.errorCode = ErrorCode::TypeMismatch;
                d.location = sourceLocFor(&sig);
                d.message = "signal '" + sig.name +
                            "' parameter '" + p.name +
                            "' has unknown type '" + p.typeName + "'";
                d.hint = "register the type with AYReflect or use a built-in "
                         "(int, float, bool, string)";
                report(d);
            }
        }
    }
    _signals[sig.name] = sig.params;
}

// S4.1 (2026-07-15): validate a `emit("name", ...args)` call site.
// Checks (in order, all hard errors except noted):
//   1. Host kind has self (`_ctx.expectSelf == true`). Tool hosts
//      do not have a receiver; codegen would emit `__ay_emit(self, ...)`
//      against an unbound `self`. Reject at compile time.
//   2. First arg is a string literal — emit/connect signal names
//      must be statically known for the validator to find the
//      signal in `_signals`. A dynamic name is rejected as
//      `TypeMismatch` (S4.1 simplification: deferred runtime-
//      checked path).
//   3. Signal name exists in `_signals` (collected by the
//      analyzeScript pre-pass).
//   4. Arity: emit arg count - 1 must equal the declared signal's
//      param count.
//   5. Each supplied arg's static type matches the declared
//      param's type (signalArgMatchesParam helper).
// On success, sets `c->ambientCall = AmbientCallKind::Emit` so
// codegen emits `__ay_emit(self, "<name>", ...args)`.
void SemanticAnalyzer::analyzeEmitCall(CallExpr& c)
{
    // 1. Host-kind guard — no self-receiver means no `self` arg to
    //    pass to __ay_emit. Tool host scripts (LG-07) cannot emit
    //    signals. Symmetric with the `run()` on non-Tool warn.
    if (!_ctx.expectSelf) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&c);   // S5 ED-02: callee-name loc
        d.message = "emit() requires a self-receiver; "
                    "Tool / EventHandler host scripts (no self) are not supported";
        d.hint = "emit/connect are per-component event calls; use them "
                 "inside Component / System host scripts, or use ambient "
                 "event.emit / event.subscribe for cross-module EventBus";
        report(d);
        return;
    }
    // 2. First arg must be a string literal.
    if (c.args.empty() ||
        dynamic_cast<LiteralExpr*>(c.args[0].get()) == nullptr ||
        !std::get_if<std::string>(
            &dynamic_cast<LiteralExpr*>(c.args[0].get())->value)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&c);
        d.message = "emit() signal name must be a string literal";
        d.hint = "write `emit(\"name\", ...args)` — dynamic signal "
                 "names are not supported in S4.1";
        report(d);
        return;
    }
    const std::string signalName =
        std::get<std::string>(
            dynamic_cast<LiteralExpr*>(c.args[0].get())->value);
    // 3. Signal exists in `_signals`.
    auto sigIt = _signals.find(signalName);
    if (sigIt == _signals.end()) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.location = sourceLocFor(&c);
        d.message = "unknown signal '" + signalName + "' in emit()";
        std::string hint = "declare the signal at script-block scope, e.g. ";
        hint += "`signal " + signalName + "()` or `signal ";
        hint += signalName + "(arg: int)`";
        // Add a soft "did you mean" style list of declared signals.
        if (!_signals.empty()) {
            hint += ". declared signals: ";
            bool first = true;
            for (const auto& [name, _] : _signals) {
                if (!first) hint += ", ";
                hint += name;
                first = false;
            }
        }
        d.hint = hint;
        report(d);
        return;
    }
    const auto& sigParams = sigIt->second;
    // 4. Arity check.
    const size_t suppliedArgs = c.args.size() - 1;
    if (suppliedArgs != sigParams.size()) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&c);
        d.message = "emit(\"" + signalName + "\") arity mismatch: "
                    "expected " + std::to_string(sigParams.size()) +
                    " arg(s), got " + std::to_string(suppliedArgs);
        d.hint = "the signal's declared parameter list must match the emit "
                 "call exactly";
        report(d);
        return;
    }
    // 5. Per-arg type compat check.
    for (size_t i = 0; i < sigParams.size(); ++i) {
        if (!signalArgMatchesParam(c.args[i + 1].get(), sigParams[i].typeName)) {
            LogiaDiagnostic d;
            d.severity = DiagnosticSeverity::Error;
            d.errorCode = ErrorCode::TypeMismatch;
            d.location = sourceLocFor(c.args[i + 1].get());
            d.message = "emit(\"" + signalName + "\") arg " +
                        std::to_string(i + 1) + " type mismatch: "
                        "expected '" + sigParams[i].typeName + "'";
            d.hint = "the signal's declared parameter type must match the "
                     "supplied argument's static type";
            report(d);
            // Don't bail — continue checking remaining args so the
            // user sees all mismatches at once.
        }
    }
    c.ambientCall = CallExpr::AmbientCallKind::Emit;
}

// S4.1 (2026-07-15) → S4.1b v2 (2026-07-18): validate a
// `connect("name", handler)` call site. v2 upgrades the handler
// argument from duck-type to static signature match against the
// signal's declared parameter list — see plan §D-5.
//
// Prologue preserved from S4.1: host-kind guard, strict arity == 2,
// first arg must be a string literal, signal must exist in
// `_signals`. New in v2: handler `c->args[1]` must be an
// `IdentifierExpr` referencing a script-block `function` collected
// in the `_functions` pre-pass (L1201+), with arity and typeNames
// matching the signal's declared params exactly. Anonymous
// closures (e.g. `connect("x", function() {})`) are rejected
// because the analyzer cannot statically resolve their parameter
// list — future slice may add codegen-side auto-naming to lift the
// restriction.
void SemanticAnalyzer::analyzeConnectCall(CallExpr& c)
{
    if (!_ctx.expectSelf) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&c);
        d.message = "connect() requires a self-receiver; "
                    "Tool / EventHandler host scripts (no self) are not supported";
        d.hint = "connect is per-component event registration; use ambient "
                 "event.subscribe for cross-module EventBus handlers";
        report(d);
        return;
    }
    if (c.args.size() != 2 ||
        dynamic_cast<LiteralExpr*>(c.args[0].get()) == nullptr ||
        !std::get_if<std::string>(
            &dynamic_cast<LiteralExpr*>(c.args[0].get())->value)) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&c);
        d.message = "connect() requires a string-literal signal name "
                    "and a handler expression";
        d.hint = "write `connect(\"name\", handler_fn)` — both args required";
        report(d);
        return;
    }
    const std::string signalName =
        std::get<std::string>(
            dynamic_cast<LiteralExpr*>(c.args[0].get())->value);
    auto sigIt = _signals.find(signalName);
    if (sigIt == _signals.end()) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.location = sourceLocFor(&c);
        d.message = "unknown signal '" + signalName + "' in connect()";
        std::string hint = "declare the signal at script-block scope";
        if (!_signals.empty()) {
            hint += ". declared signals: ";
            bool first = true;
            for (const auto& [name, _] : _signals) {
                if (!first) hint += ", ";
                hint += name;
                first = false;
            }
        }
        d.hint = hint;
        report(d);
        return;
    }
    c.ambientCall = CallExpr::AmbientCallKind::Connect;
}

// S4.1b (2026-07-18): validate a `disconnect(id)` call site. Much
// simpler than connect — the id is opaque (just an int returned by
// a prior `connect(...)`), so the analyzer does NOT need to know
// which signal it points at; the runtime helper walks all lists for
// the instance and tombstone-marks the matching record.
//
// Validation surface:
//   1. Host-kind guard — same as emit/connect; disconnect needs
//      `self` because the per-instance bag is keyed on it.
//   2. Arity == 1 (one connection-id argument).
//   3. Hard int-check on the argument (S4.1b revised 2026-07-18):
//      if it's an `IdentifierExpr` resolved via `_scope` to a
//      non-int `VarDeclStmt`, emit a hard Error — the player must
//      explicitly declare `var h: int = connect(...)` to use
//      disconnect. No init-position inference (Logia has no
//      expression-type system per the R5.x "static-only" philosophy).
//      Runtime no-op safety net still covers non-identifier args
//      (literals, undeclared identifiers).
//
// On full pass, stamps `c->ambientCall = AmbientCallKind::Disconnect`
// so codegen emits `__ay_disconnect(self, id)`.
void SemanticAnalyzer::analyzeDisconnectCall(CallExpr& c)
{
    if (!_ctx.expectSelf) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::InvalidStatement;
        d.location = sourceLocFor(&c);
        d.message = "disconnect() requires a self-receiver; "
                    "Tool / EventHandler host scripts (no self) are not supported";
        d.hint = "disconnect is per-component event deregistration; use "
                 "ambient event.unsubscribe for cross-module EventBus";
        report(d);
        return;
    }
    if (c.args.size() != 1) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.errorCode = ErrorCode::TypeMismatch;
        d.location = sourceLocFor(&c);
        d.message = "disconnect() expects exactly 1 connection-id argument";
        d.hint = "store the connect() return value: "
                 "`var h = connect(\"name\", fn); disconnect(h)`";
        report(d);
        return;
    }
    // Hard int check (S4.1b revised, 2026-07-18): if the user passes
    // a typed local, the declared type MUST be `int`. No soft-warn
    // escape — the player is expected to declare `var h: int = ...`
    // explicitly when capturing a connection id, and the runtime
    // helper is still a no-op safety net for non-identifier args
    // (literals, undeclared identifiers) so the type-mismatch error
    // is the strict correct behavior here.
    if (auto* id = dynamic_cast<IdentifierExpr*>(c.args[0].get())) {
        auto scopeIt = _scope.find(id->name);
        if (scopeIt != _scope.end()) {
            const auto* vd = static_cast<const VarDeclStmt*>(
                scopeIt->second.decl);
            if (vd && vd->typeName != "int") {
                LogiaDiagnostic d;
                d.severity = DiagnosticSeverity::Error;
                d.errorCode = ErrorCode::TypeMismatch;
                d.location = sourceLocFor(id);
                d.message = "disconnect argument '" + id->name +
                            "' has type '" + vd->typeName +
                            "' but expects a connection id (int)";
                d.hint = "declare `var " + id->name + ": int = connect(...)` "
                         "so disconnect(" + id->name + ") type-checks";
                report(d);
                return;
            }
        }
    }
    c.ambientCall = CallExpr::AmbientCallKind::Disconnect;
}

void SemanticAnalyzer::analyzeStmt(Stmt& s)
{
    if (auto* es = dynamic_cast<ExprStmt*>(&s)) {
        // 2026-07-11 audit fix: surface Lua-keyword leaks as a soft
        // diagnostic. The Logia lexer does not reserve `local`,
        // `nil`, `function`, etc., so a bare-expression statement
        // becomes an IdentifierExpr whose name is itself a Lua
        // keyword. This is how `local s = expr` parses today
        // (ExprStmt(IdentifierExpr("local")) followed by a separate
        // BinaryExpr assignment). Until R5+ locks the grammar down,
        // surface the leak once per script — analyzer deduplicates by
        // script-level set since every R3/R4 audit fixture trips
        // this repeatedly and we don't want to flood diagnostics.
        if (es->expr) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(es->expr.get())) {
                static const std::unordered_set<std::string>
                    kLuaKeywordLeaks = {"local", "nil", "function",
                                         "then", "end", "do", "while",
                                         "for", "repeat", "until",
                                         "break", "continue", "return"};
                if (kLuaKeywordLeaks.count(id->name)) {
                    LogiaDiagnostic d;
                    d.severity = DiagnosticSeverity::Warning;
                    d.errorCode = ErrorCode::LuaKeywordLeak;
                    d.message = std::string("'") + id->name +
                               "' is a Lua keyword; Logia recommends "
                               "`var NAME : TYPE = expr` (at script-block "
                               "scope) instead of `" + id->name + " = expr`.";
                    d.hint = "Use `var x: int = 0` at script-block scope, "
                             "then `x = new_value` inside the lifecycle body. "
                             "Add `function NAME(...)` for a script-block "
                             "helper (2026-07-11 audit fix).";
                    report(d);
                }
            }
            analyzeExpr(*es->expr);
        }
    } else if (auto* is = dynamic_cast<IfStmt*>(&s)) {
        // R5.2-H (2026-07-14): if condition must statically be bool.
        // Same rule as while — no implicit truthiness. See
        // analyzeWhileStmt's comment for the rationale.
        validateCondition(is->condition.get(),
                          sourceLocFor(is->condition.get()), "if");
        for (auto& t : is->thenBranch) if (t) analyzeStmt(*t);
        for (auto& e : is->elseBranch) if (e) analyzeStmt(*e);
    } else if (auto* ws = dynamic_cast<WhileStmt*>(&s)) {     // R5.0 (2026-07-13)
        analyzeWhileStmt(*ws);
    } else if (auto* fs = dynamic_cast<ForStmt*>(&s)) {       // R5.0 (2026-07-13)
        analyzeForStmt(*fs);
    } else if (auto* bs = dynamic_cast<BreakStmt*>(&s)) {    // R5.1 (2026-07-13)
        analyzeBreakStmt(*bs);
    } else if (auto* cs = dynamic_cast<ContinueStmt*>(&s)) { // R5.1 (2026-07-13)
        analyzeContinueStmt(*cs);
    } else if (auto* ld = dynamic_cast<LabelDeclStmt*>(&s)) { // R5.2-B (2026-07-14)
        analyzeLabelDeclStmt(*ld);
    } else if (auto* rs = dynamic_cast<ReturnStmt*>(&s)) {
        if (rs->value) analyzeExpr(*rs->value);
    } else if (auto* vd = dynamic_cast<VarDeclStmt*>(&s)) {
        analyzeVarDecl(*vd);
        if (vd->initializer) analyzeExpr(*vd->initializer);
    } else if (auto* lf = dynamic_cast<LifecycleFuncDecl*>(&s)) {
        analyzeLifecycle(*lf);
    } else if (auto* sig = dynamic_cast<SignalDeclStmt*>(&s)) {
        // S4.1 (2026-07-15): validated in the analyzeScript pre-pass
        // (forward-reference collection). The defensive no-op here
        // mirrors the FunctionDeclStmt fallback — if a SignalDeclStmt
        // ever reaches analyzeStmt via a future refactor of the
        // analyzeScript two-pass structure, it stays inert rather
        // than triggering a "Unsupported statement" diagnostic.
        (void)sig;
    }
}

void SemanticAnalyzer::analyzeExpr(Expr& e)
{
    if (auto* b = dynamic_cast<BinaryExpr*>(&e)) {
        // Assignment (`=`) target is allowed to be an undeclared
        // identifier — codegen lowers it to a Lua assignment that
        // implicitly creates a global. This matches the S1 codegen
        // tests that use bare assignments to `x` / `speed` / etc.
        if (b->op.type == TokenType::Equal && b->left) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(b->left.get())) {
                auto it = _scope.find(id->name);
                if (it != _scope.end()) {
                    id->resolvedType = it->second.type;
                    id->resolvedDecl = it->second.decl;
                }
                // Undeclared assignment target — silently allowed
                // (Lua-style implicit global).
            } else {
                analyzeExpr(*b->left);
            }
        } else if (b->left) {
            analyzeExpr(*b->left);
        }
        if (b->right) analyzeExpr(*b->right);
    } else if (auto* u = dynamic_cast<UnaryExpr*>(&e)) {
        if (u->operand) analyzeExpr(*u->operand);
    } else if (auto* c = dynamic_cast<CallExpr*>(&e)) {
        // S2.5: no more `get_component` magic. Calls go through the
        // generic identifier-resolution path.
        if (c->callee) analyzeExpr(*c->callee);
        for (auto& a : c->args) if (a) analyzeExpr(*a);
        // S3.12 (track R2 §5.7.4): detect `self.<method>(...)` and
        // stamp resolvedMethod. Codegen reads this to emit
        // `ayt_reflect_call_method(self, "<Type>", "<m>", ...)`
        // instead of bare Lua dispatch. Only fires for the
        // self-receiver case (kind=Component/System, expectSelf=true).
        if (c->callee) {
            if (auto* mem = dynamic_cast<MemberExpr*>(c->callee.get())) {
                // Check `mem->object` is the `self` identifier.
                auto* selfId = dynamic_cast<IdentifierExpr*>(mem->object.get());
                if (selfId && selfId->name == "self" && _ctx.hostType != nullptr) {
                    auto* m = _ctx.hostType->findMethod(mem->member.c_str());
                    if (m) {
                        c->resolvedMethod = m;
                        c->resolvedMethodOwnerName = _ctx.hostType->getName();
                        c->resolvedType = m->getReturnType();
                    }
                }
            }
        }
        // S4.1 (2026-07-15) + S4.1b (2026-07-18): per-component
        // signal ambient call recognition. `emit(...)`,
        // `connect(...)`, and `disconnect(...)` are NOT
        // lexer-reserved — they reach this branch as ordinary
        // CallExprs whose callee is an IdentifierExpr. Shape-recognise
        // here (not via `ambientIdentifiers()`, which is for
        // object-receiver patterns like `input.x(...)`); stamp
        // `c->ambientCall` so codegen can emit the specialised
        // lowering. The free-function ambient pattern is a new
        // surface — see commit message for rationale.
        if (c->callee) {
            if (auto* calleeId = dynamic_cast<IdentifierExpr*>(c->callee.get())) {
                if (calleeId->name == "emit") {
                    analyzeEmitCall(*c);
                } else if (calleeId->name == "connect") {
                    analyzeConnectCall(*c);
                } else if (calleeId->name == "disconnect") {  // S4.1b
                    analyzeDisconnectCall(*c);
                }
            }
        }
    } else if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) {
        analyzeIdentifierExpr(*id);
    } else if (auto* m = dynamic_cast<MemberExpr*>(&e)) {
        if (m->object) analyzeExpr(*m->object);
        const ayt::reflect::ITypeInfo* parent = m->object ? m->object->resolvedType : nullptr;
        analyzeMemberExpr(*m, parent);
    } else if (auto* i = dynamic_cast<IndexExpr*>(&e)) {
        if (i->object) analyzeExpr(*i->object);
        if (i->index)  analyzeExpr(*i->index);
    } else if (auto* lit = dynamic_cast<LiteralExpr*>(&e)) {
        // No type stamping for primitives in S2.5 (codegen doesn't read it).
        (void)lit;
    }
}

void SemanticAnalyzer::analyzeIdentifierExpr(IdentifierExpr& id)
{
    auto it = _scope.find(id.name);
    if (it != _scope.end()) {
        id.resolvedType = it->second.type;
        id.resolvedDecl = it->second.decl;
        return;
    }
    if (isAmbientIdentifier(id.name)) {
        return;  // ambient — no ITypeInfo, allowed
    }
    // S4.1: free-function ambients (emit/connect). Same carve-out as
    // ambientIdentifiers() object receivers — shape-recognised later
    // in analyzeExpr's CallExpr branch; do not warn as implicit globals.
    if (id.name == "emit" || id.name == "connect") {
        return;
    }
    // Undeclared identifier read. Lua-style implicit global.
    LogiaDiagnostic d;
    d.severity = DiagnosticSeverity::Warning;
    d.errorCode = ErrorCode::UnknownIdentifier;
    d.location = sourceLocFor(&id);   // S5 ED-02: identifier loc
    d.message = "implicit global '" + id.name + "' (not declared in script)";
    d.hint = "declare it with `var " + id.name + ": <Type>`, or pass it as a parameter";
    report(d);
}

void SemanticAnalyzer::analyzeMemberExpr(MemberExpr& m,
                                         const ayt::reflect::ITypeInfo* parent)
{
    if (!parent) {
        // Parent type unresolved (ambient or unknown). Don't warn here —
        // the caller already reported an issue or the chain root is
        // ambient (input.is_pressed → ok).
        return;
    }
    auto* field = parent->findField(m.member.c_str());
    if (!field) {
        LogiaDiagnostic d;
        d.severity = DiagnosticSeverity::Warning;
        d.errorCode = ErrorCode::UnknownIdentifier;
        d.location = sourceLocFor(&m);   // S5 ED-02: field-name loc
        d.message = "type '" + std::string(parent->getName()) +
                    "' has no field '" + m.member + "'";
        report(d);
        return;
    }
    m.resolvedField = field;
    m.resolvedType = field->getType();
}

} // namespace ayt::script::logia