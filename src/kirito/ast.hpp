#ifndef KIRITO_AST_HPP
#define KIRITO_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common.hpp"

namespace kirito::ast {

// The AST is the stable contract between the front end and the evaluator. Nodes carry only
// literal payloads, child pointers, and a source span — nothing from the value/VM layer. The
// evaluator visits via accept(); a future bytecode compiler could be a second visitor.

struct LiteralExpr;
struct NameExpr;
struct UnaryExpr;
struct BinaryExpr;
struct LogicalExpr;
struct ConditionalExpr;
struct CallExpr;
struct FunctionExpr;
struct MemberExpr;
struct IndexExpr;
struct SliceExpr;
struct ListLiteral;
struct SetLiteral;
struct DictLiteral;
struct FStringExpr;
struct TupleExpr;
struct StarExpr;

struct ExprVisitor {
    virtual ~ExprVisitor() = default;
    virtual void visit(const LiteralExpr&) = 0;
    virtual void visit(const NameExpr&) = 0;
    virtual void visit(const UnaryExpr&) = 0;
    virtual void visit(const BinaryExpr&) = 0;
    virtual void visit(const LogicalExpr&) = 0;
    virtual void visit(const ConditionalExpr&) = 0;
    virtual void visit(const CallExpr&) = 0;
    virtual void visit(const FunctionExpr&) = 0;
    virtual void visit(const MemberExpr&) = 0;
    virtual void visit(const IndexExpr&) = 0;
    virtual void visit(const SliceExpr&) = 0;
    virtual void visit(const ListLiteral&) = 0;
    virtual void visit(const SetLiteral&) = 0;
    virtual void visit(const DictLiteral&) = 0;
    virtual void visit(const FStringExpr&) = 0;
    virtual void visit(const TupleExpr&) = 0;
    virtual void visit(const StarExpr&) = 0;
};

// A cheap tag for assignment-target dispatch, avoiding dynamic_cast on the hot path.
enum class ExprKind { Other, Name, Index, Member, Tuple, Star };

struct Expr {
    SourceSpan span;
    virtual ~Expr() = default;
    virtual void accept(ExprVisitor&) const = 0;
    virtual ExprKind exprKind() const { return ExprKind::Other; }
};
using ExprPtr = std::unique_ptr<Expr>;

// std::monostate == the None literal.
struct LiteralExpr : Expr {
    std::variant<std::monostate, int64_t, double, bool, std::string> value;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct NameExpr : Expr {
    std::string name;
    ExprKind exprKind() const override { return ExprKind::Name; }
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct UnaryExpr : Expr {
    UnOp op;
    ExprPtr operand;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct BinaryExpr : Expr {
    BinOp op;
    ExprPtr lhs;
    ExprPtr rhs;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// `and` / `or` — separate from BinaryExpr because they short-circuit and yield an operand.
struct LogicalExpr : Expr {
    bool isAnd;  // true == `and`, false == `or`
    ExprPtr lhs;
    ExprPtr rhs;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// `then if cond else orelse` — a conditional expression. Like `and`/`or` it short-circuits: only
// the selected branch is evaluated.
struct ConditionalExpr : Expr {
    ExprPtr then;     // value if cond is truthy
    ExprPtr cond;
    ExprPtr orelse;   // value if cond is falsy
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// A call argument: positional (name empty) or named (name set, for `f(key=value)`).
struct Arg {
    std::string name;  // "" for positional
    ExprPtr value;
};
struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<Arg> args;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct MemberExpr : Expr {
    ExprPtr object;
    std::string name;
    ExprKind exprKind() const override { return ExprKind::Member; }
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct IndexExpr : Expr {
    ExprPtr object;
    std::vector<ExprPtr> indices;  // obj[a, b, c] -> multiple keys
    ExprKind exprKind() const override { return ExprKind::Index; }
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// obj[start:stop:step] — any of start/stop/step may be null (omitted).
struct SliceExpr : Expr {
    ExprPtr object;
    ExprPtr start;
    ExprPtr stop;
    ExprPtr step;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct ListLiteral : Expr {
    std::vector<ExprPtr> elems;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// A comma sequence with no surrounding brackets: `a, b` / `return a, b` / `a, b = ...`. As a value
// (packing) it evaluates to a List; as an assignment target (its elements being Name/Index/Member,
// or a StarExpr) it drives unpacking.
struct TupleExpr : Expr {
    std::vector<ExprPtr> elems;
    ExprKind exprKind() const override { return ExprKind::Tuple; }
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// `*target` — a starred unpack target that absorbs the remaining items as a List.
struct StarExpr : Expr {
    ExprPtr inner;
    ExprKind exprKind() const override { return ExprKind::Star; }
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct SetLiteral : Expr {
    std::vector<ExprPtr> elems;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct DictLiteral : Expr {
    std::vector<std::pair<ExprPtr, ExprPtr>> entries;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// f"text {expr} ...": alternating literal text and embedded expressions.
struct FStringExpr : Expr {
    struct Part {
        bool isExpr = false;
        std::string literal;
        ExprPtr expr;
        std::string spec;  // optional :format-spec applied to the expression's value (f"{x:05d}")
    };
    std::vector<Part> parts;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

struct ExprStmt;
struct VarDeclStmt;
struct AssignStmt;
struct IfStmt;
struct WhileStmt;
struct ForStmt;
struct BreakStmt;
struct ContinueStmt;
struct ReturnStmt;
struct TryStmt;
struct ThrowStmt;
struct ClassStmt;
struct WithStmt;
struct PassStmt;
struct TodoStmt;
struct AssertStmt;
struct DiscardStmt;
struct SwitchStmt;

struct StmtVisitor {
    virtual ~StmtVisitor() = default;
    virtual void visit(const ExprStmt&) = 0;
    virtual void visit(const VarDeclStmt&) = 0;
    virtual void visit(const AssignStmt&) = 0;
    virtual void visit(const IfStmt&) = 0;
    virtual void visit(const WhileStmt&) = 0;
    virtual void visit(const ForStmt&) = 0;
    virtual void visit(const BreakStmt&) = 0;
    virtual void visit(const ContinueStmt&) = 0;
    virtual void visit(const ReturnStmt&) = 0;
    virtual void visit(const TryStmt&) = 0;
    virtual void visit(const ThrowStmt&) = 0;
    virtual void visit(const ClassStmt&) = 0;
    virtual void visit(const WithStmt&) = 0;
    virtual void visit(const PassStmt&) = 0;
    virtual void visit(const TodoStmt&) = 0;
    virtual void visit(const AssertStmt&) = 0;
    virtual void visit(const DiscardStmt&) = 0;
    virtual void visit(const SwitchStmt&) = 0;
};

struct Stmt {
    SourceSpan span;
    virtual ~Stmt() = default;
    virtual void accept(StmtVisitor&) const = 0;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct ExprStmt : Stmt {
    ExprPtr expr;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `discard EXPR` — evaluate EXPR for its side effects and intentionally drop its value. Suppresses
// the "return value ignored" warning; otherwise identical to an ExprStmt at runtime.
struct DiscardStmt : Stmt {
    ExprPtr expr;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `var NAME = init` — declares NAME in the current scope.
// `var a = e` or, with unpacking, `var a, b = e` / `var a, *rest = e`. `names` always holds the
// declared name(s); `starIndex` is the position of a `*name` (or -1). Single-name decls keep one
// entry and starIndex -1.
struct VarDeclStmt : Stmt {
    std::vector<std::string> names;
    int starIndex = -1;
    ExprPtr init;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `target = value` — rebinds an existing name (target is a NameExpr for now).
struct AssignStmt : Stmt {
    ExprPtr target;
    ExprPtr value;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

using Block = std::vector<StmtPtr>;

// One function parameter: a name, an optional type annotation, and an optional default value.
//   Function(a, b: Dict, c = 5, d: Float = 1.0):
// The annotation is the bare type/class name (empty if none); enforcement is done at call time.
struct Param {
    std::string name;
    std::string annotation;  // "" if unannotated
    ExprPtr defaultValue;    // null if no default
};

// First-class function literal: `Function(params) [-> RetType]: <indented block>`. The body is an
// owned block; the evaluator captures the defining scope to make a closure. (Defined here, after
// Block, so its members are complete.)
struct FunctionExpr : Expr {
    std::vector<Param> params;
    std::string returnAnnotation;  // "" if no `-> Type`
    std::string name;              // binding name (`var NAME = Function`/method), "" if anonymous; used
                                   // only to label tracebacks — name resolution never depends on it
    Block body;
    // Evaluator-side memo: true once we've determined this function has no param/return annotations,
    // enabling a no-temporaries fast bind for positional, exact-arity calls. Computed lazily.
    mutable std::optional<bool> fastBindable;
    void accept(ExprVisitor& v) const override { v.visit(*this); }
};

// `return [value]` — value is null for a bare `return`.
struct ReturnStmt : Stmt {
    ExprPtr value;  // may be null
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `if cond: ... [elif cond: ...] [else: ...]` — one entry per if/elif branch, plus optional else.
struct IfStmt : Stmt {
    std::vector<std::pair<ExprPtr, Block>> branches;
    std::optional<Block> orelse;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    Block body;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `for var in iterable: <block>` — var is bound in the enclosing scope.
// `for v in it` or, with unpacking, `for k, v in it` / `for a, *rest in it`. `vars` holds the loop
// name(s); `starIndex` marks a `*name` (or -1). Single-variable loops keep one entry, starIndex -1.
struct ForStmt : Stmt {
    std::vector<std::string> vars;
    int starIndex = -1;
    ExprPtr iterable;
    Block body;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// One `catch [type] [as name]:` arm. type is null for a catch-all; name is empty for no binding.
struct CatchClause {
    ExprPtr type;
    std::string name;
    Block body;
};

// `try: ... catch ...: ... [finally: ...]`.
struct TryStmt : Stmt {
    Block body;
    std::vector<CatchClause> handlers;
    bool hasFinally = false;
    Block finallyBody;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `throw value`.
struct ThrowStmt : Stmt {
    ExprPtr value;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `class Name [(Base)]: <method defs>`.
struct ClassStmt : Stmt {
    std::string name;
    ExprPtr base;  // optional base class
    Block body;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `with context [as name]: <body>` — calls context.enter() (bound to name), runs the body, and
// always calls context.exit().
struct WithStmt : Stmt {
    ExprPtr context;
    std::string name;  // empty for no binding
    Block body;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

struct BreakStmt : Stmt {
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

struct ContinueStmt : Stmt {
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

struct PassStmt : Stmt {
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `todo [message]` — a no-op at runtime (like pass), but the analyzer emits a warning at its
// location reminding you to implement something. `message` is the optional trailing string.
struct TodoStmt : Stmt {
    std::string message;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `assert cond [, message]` — throws if cond is falsy.
struct AssertStmt : Stmt {
    ExprPtr cond;
    ExprPtr message;  // optional
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

// `switch SUBJECT:` with `case V[, V2...]:` arms and an optional `default:`. No fallthrough — exactly
// one arm runs. Case values are constant literals; the compiler lowers the switch to an exact-match
// comparison chain (see compiler.hpp), and values may mix types (case 1 / case "x" / case True).
struct CaseClause {
    std::vector<ExprPtr> values;  // one or more literal values selecting this arm
    Block body;
};
struct SwitchStmt : Stmt {
    ExprPtr subject;
    std::vector<CaseClause> cases;
    bool hasDefault = false;
    Block defaultBody;
    void accept(StmtVisitor& v) const override { v.visit(*this); }
};

struct Program {
    Block stmts;
};

// Walk an assignment target's structure: invoke `onName` for each bare-name leaf, and `onSubExpr` for
// each sub-expression merely READ to locate the target (an index/member object, the index keys).
// Tuple/star targets recurse. Shared by the resolver (name-resolution) and the analyzer (use-tracking),
// which differ only in those two per-node actions.
template <typename OnName, typename OnSubExpr>
inline void walkAssignTarget(const Expr& target, OnName&& onName, OnSubExpr&& onSubExpr) {
    switch (target.exprKind()) {
        case ExprKind::Name: onName(static_cast<const NameExpr&>(target)); break;
        case ExprKind::Tuple:
            for (const auto& e : static_cast<const TupleExpr&>(target).elems)
                walkAssignTarget(*e, onName, onSubExpr);
            break;
        case ExprKind::Star:
            walkAssignTarget(*static_cast<const StarExpr&>(target).inner, onName, onSubExpr);
            break;
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(target);
            onSubExpr(*ix.object);
            for (const auto& k : ix.indices) onSubExpr(*k);
        } break;
        case ExprKind::Member: onSubExpr(*static_cast<const MemberExpr&>(target).object); break;
        default: break;
    }
}

}  // namespace kirito::ast

#endif
