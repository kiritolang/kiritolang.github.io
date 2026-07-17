#ifndef KIRITO_PARSER_HPP
#define KIRITO_PARSER_HPP

#include <cctype>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "common.hpp"
#include "lexer.hpp"

namespace kirito {

// Recursive descent: statements at the top, expressions by precedence level
// (comparison < add < mul < unary < pow < primary), with ** right-associative — so -2**2 == -4
// and 2**3**2 == 512.
class Parser {
public:
    // `startDepth` seeds the expression-nesting counter — used when re-parsing an f-string's embedded
    // expression so nested f-strings (`f"{f'{...}'}"`) accumulate toward kMaxParseDepth instead of
    // resetting to 0 each level and overflowing the native stack.
    explicit Parser(std::vector<Token> tokens, int startDepth = 0)
        : toks_(std::move(tokens)), exprDepth_(startDepth) {}
    // With the newline-normalized source (from Lexer::source()): the parser then captures the verbatim
    // source text of every Function/class literal into its AST node (FunctionExpr/ClassStmt::source),
    // which is what makes those values serializable by default. `startDepth` is as above.
    Parser(std::vector<Token> tokens, std::string source, int startDepth = 0)
        : toks_(std::move(tokens)), source_(std::move(source)), exprDepth_(startDepth) {
        buildLineIndex();
    }

    ast::Program parseProgram() {
        ast::Program prog;
        while (!at(TokenType::EndOfFile)) {
            if (at(TokenType::Newline)) { advance(); continue; }
            prog.stmts.push_back(parseStatement());
        }
        return prog;
    }

private:
    const Token& peek() const { return toks_[pos_]; }
    const Token& peekAt(std::size_t k) const {
        return toks_[std::min(pos_ + k, toks_.size() - 1)];
    }
    bool at(TokenType t) const { return peek().type == t; }
    // True if the current token is an identifier with the given text — used for soft keywords
    // (`case`/`default`) that are only special in a specific syntactic position.
    bool atWord(const char* w) const { return at(TokenType::Identifier) && peek().text == w; }
    const Token& advance() { return toks_[pos_++]; }

    const Token& expect(TokenType t, const char* what) {
        if (!at(t)) throw KiritoError(std::string("expected ") + what, peek().span);
        return advance();
    }
    void expectStatementEnd() {
        if (at(TokenType::Newline)) { advance(); return; }
        if (at(TokenType::EndOfFile)) return;
        throw KiritoError("expected end of statement", peek().span);
    }

    // A simple statement (var/assign/expr/return) is normally newline-terminated, but when its
    // expression ended in an indented block (a Function literal), the closing dedent already
    // terminated it — so no trailing newline is required.
    void endSimpleStatement() {
        if (blockJustClosed_) { blockJustClosed_ = false; return; }
        expectStatementEnd();
    }

    ast::StmtPtr parseStatement() {
        blockJustClosed_ = false;
        switch (peek().type) {
            case TokenType::KwVar: { return parseVarDecl(); } break;
            case TokenType::KwIf: { return parseIf(); } break;
            case TokenType::KwWhile: { return parseWhile(); } break;
            case TokenType::KwFor: { return parseFor(); } break;
            case TokenType::KwTry: { return parseTry(); } break;
            case TokenType::KwThrow: { return parseThrow(); } break;
            case TokenType::KwSwitch: { return parseSwitch(); } break;
            case TokenType::KwClass: { return parseClass(); } break;
            case TokenType::KwWith: { return parseWith(); } break;
            case TokenType::KwReturn: { return parseReturn(); } break;
            case TokenType::KwPass: {
                auto node = std::make_unique<ast::PassStmt>();
                node->span = advance().span;
                expectStatementEnd();
                return node;
            } break;
            case TokenType::KwTodo: {
                auto node = std::make_unique<ast::TodoStmt>();
                node->span = advance().span;
                if (at(TokenType::String)) node->message = advance().text;  // optional reminder
                expectStatementEnd();
                return node;
            } break;
            case TokenType::KwDiscard: {
                auto node = std::make_unique<ast::DiscardStmt>();
                node->span = advance().span;
                node->expr = parseExpr();
                endSimpleStatement();
                return node;
            } break;
            case TokenType::KwAssert: {
                auto node = std::make_unique<ast::AssertStmt>();
                node->span = advance().span;
                node->cond = parseExpr();
                if (at(TokenType::Comma)) { advance(); node->message = parseExpr(); }
                endSimpleStatement();
                return node;
            } break;
            case TokenType::KwBreak: {
                auto node = std::make_unique<ast::BreakStmt>();
                node->span = advance().span;
                if (loopDepth_ == 0) throw KiritoError("'break' outside loop", node->span);
                expectStatementEnd();
                return node;
            } break;
            case TokenType::KwContinue: {
                auto node = std::make_unique<ast::ContinueStmt>();
                node->span = advance().span;
                if (loopDepth_ == 0) throw KiritoError("'continue' outside loop", node->span);
                expectStatementEnd();
                return node;
            } break;
            default: { break; }
        }

        SourceSpan span = peek().span;
        auto expr = parseTargetElement();
        if (at(TokenType::Comma)) {
            // A comma sequence at statement level: unpack targets (`a, b = ...`) or a bare tuple.
            auto tup = std::make_unique<ast::TupleExpr>();
            tup->span = span;
            tup->elems.push_back(std::move(expr));
            while (at(TokenType::Comma)) {
                advance();
                if (at(TokenType::Assign) || at(TokenType::Newline) ||
                    at(TokenType::EndOfFile) || at(TokenType::Dedent)) break;  // trailing comma
                tup->elems.push_back(parseTargetElement());
            }
            expr = std::move(tup);
        }
        if (at(TokenType::Assign)) {
            advance();
            auto value = parseValueSeq();
            endSimpleStatement();
            auto node = std::make_unique<ast::AssignStmt>();
            node->span = span;
            if (auto* nameTgt = dynamic_cast<ast::NameExpr*>(expr.get()))
                labelFunction(nameTgt->name, value.get());
            node->target = std::move(expr);
            node->value = std::move(value);
            return node;
        }
        endSimpleStatement();
        auto node = std::make_unique<ast::ExprStmt>();
        node->span = span;
        node->expr = std::move(expr);
        return node;
    }

    // One element of a statement-level target list: a normal expression or a `*target`.
    ast::ExprPtr parseTargetElement() {
        if (at(TokenType::Star)) {
            auto star = std::make_unique<ast::StarExpr>();
            star->span = advance().span;
            star->inner = parseExpr();
            return star;
        }
        return parseExpr();
    }

    ast::StmtPtr parseSwitch() {
        auto node = std::make_unique<ast::SwitchStmt>();
        node->span = advance().span;  // 'switch'
        node->subject = parseExpr();
        expect(TokenType::Colon, "':' after the switch subject");
        expect(TokenType::Newline, "a newline before the switch body");
        expect(TokenType::Indent, "an indented switch body");
        // The body is a sequence of `case ...:` arms and an optional trailing `default:`. `case` and
        // `default` are soft keywords: matched here as identifiers so they remain usable as names
        // (e.g. a `default` parameter) outside a switch.
        while (!at(TokenType::Dedent) && !at(TokenType::EndOfFile)) {
            if (at(TokenType::Newline)) { advance(); continue; }
            if (atWord("case")) {
                advance();
                ast::CaseClause clause;
                clause.values.push_back(parseExpr());
                while (at(TokenType::Comma)) { advance(); clause.values.push_back(parseExpr()); }
                clause.body = parseBlock();
                node->cases.push_back(std::move(clause));
            } else if (atWord("default")) {
                if (node->hasDefault) throw KiritoError("a switch can have only one 'default'", peek().span);
                advance();
                node->hasDefault = true;
                node->defaultBody = parseBlock();
            } else {
                throw KiritoError("expected 'case' or 'default' in switch body", peek().span);
            }
        }
        expect(TokenType::Dedent, "a dedent to close the switch body");
        blockJustClosed_ = true;
        if (node->cases.empty() && !node->hasDefault)
            throw KiritoError("'switch' needs at least one 'case' or a 'default'", node->span);
        return node;
    }

    ast::StmtPtr parseTry() {
        auto node = std::make_unique<ast::TryStmt>();
        node->span = advance().span;  // 'try'
        node->body = parseBlock();
        while (at(TokenType::KwCatch)) {
            advance();
            ast::CatchClause clause;
            if (!at(TokenType::Colon) && !at(TokenType::KwAs)) clause.type = parseExpr();
            if (at(TokenType::KwAs)) {
                advance();
                clause.name = expect(TokenType::Identifier, "a name after 'as'").text;
            }
            clause.body = parseBlock();
            node->handlers.push_back(std::move(clause));
        }
        if (at(TokenType::KwFinally)) {
            advance();
            node->hasFinally = true;
            node->finallyBody = parseBlock();
        }
        if (node->handlers.empty() && !node->hasFinally)
            throw KiritoError("'try' needs at least one 'catch' or a 'finally'", node->span);
        return node;
    }

    ast::StmtPtr parseClass() {
        auto node = std::make_unique<ast::ClassStmt>();
        std::size_t startTok = pos_;                    // the 'class' keyword — start of the construct
        node->span = advance().span;  // 'class'
        node->name = expect(TokenType::Identifier, "a class name").text;
        if (at(TokenType::LParen)) {
            advance();
            node->base = parseExpr();
            expect(TokenType::RParen, "')' after base class");
        }
        node->body = parseBlock();
        node->source = captureSource(startTok);  // verbatim text, for serialization
        return node;
    }

    ast::StmtPtr parseWith() {
        auto node = std::make_unique<ast::WithStmt>();
        node->span = advance().span;  // 'with'
        node->context = parseExpr();
        if (at(TokenType::KwAs)) {
            advance();
            node->name = expect(TokenType::Identifier, "a name after 'as'").text;
        }
        node->body = parseBlock();
        return node;
    }

    ast::StmtPtr parseThrow() {
        auto node = std::make_unique<ast::ThrowStmt>();
        node->span = advance().span;  // 'throw'
        node->value = parseExpr();
        endSimpleStatement();
        return node;
    }

    ast::StmtPtr parseReturn() {
        auto node = std::make_unique<ast::ReturnStmt>();
        node->span = advance().span;  // 'return'
        if (funcDepth_ == 0) throw KiritoError("'return' outside function", node->span);
        if (!at(TokenType::Newline) && !at(TokenType::EndOfFile))
            node->value = parseValueSeq();  // `return a, b` packs into a List
        endSimpleStatement();
        return node;
    }

    // If `value` is a Function literal bound to a single name (`var NAME = Function(...)`,
    // `NAME = Function(...)`, or a class method), record the name on it so a traceback can label the
    // frame. Cosmetic only — name resolution never reads it.
    static void labelFunction(const std::string& name, ast::Expr* value) {
        if (auto* fn = dynamic_cast<ast::FunctionExpr*>(value))
            if (fn->name.empty()) fn->name = name;
    }

    ast::StmtPtr parseVarDecl() {
        SourceSpan span = advance().span;  // 'var'
        auto node = std::make_unique<ast::VarDeclStmt>();
        node->span = span;
        parseTargetNameList(node->names, node->starIndex, "a name after 'var'");
        expect(TokenType::Assign, "'=' in var declaration");
        node->init = parseValueSeq();
        if (node->names.size() == 1 && node->starIndex == -1) labelFunction(node->names[0], node->init.get());
        endSimpleStatement();
        return node;
    }

    // Parse a comma-separated list of (optionally one starred) names, used by `var` and `for`.
    void parseTargetNameList(std::vector<std::string>& names, int& starIndex, const char* what) {
        while (true) {
            if (at(TokenType::Star)) {
                if (starIndex != -1) throw KiritoError("two starred targets in assignment", peek().span);
                advance();
                starIndex = static_cast<int>(names.size());
            }
            names.push_back(expect(TokenType::Identifier, what).text);
            if (!at(TokenType::Comma)) break;
            advance();
            if (at(TokenType::Assign) || at(TokenType::KwIn)) break;  // trailing comma
        }
    }

    // Parse a value position that may pack: `e` or `e, e, ...` (a tuple, evaluated to a List).
    ast::ExprPtr parseValueSeq() {
        SourceSpan span = peek().span;
        auto first = parseExpr();
        // A block-bodied Function literal self-terminates at its dedent (like endSimpleStatement), so a
        // following `,` begins the NEXT statement and must not comma-pack the function value (A02-1).
        if (!at(TokenType::Comma) || blockJustClosed_) return first;
        // A bare comma right after an INLINE-bodied function literal reads ambiguously: the `, b` in
        // `var f = Function(): return a, b` was silently absorbed into a top-level pack, binding f to
        // the List [<function>, b] (and the function returning only `a`). Inline functions don't pack —
        // reject it with a clear diagnostic. (A call-arg or bracketed list, which delimit, are fine and
        // don't go through parseValueSeq; wrap the function in [ ] if a List of it and more is intended.)
        rejectInlineFnPack(*first);
        auto tup = std::make_unique<ast::TupleExpr>();
        tup->span = span;
        tup->elems.push_back(std::move(first));
        while (at(TokenType::Comma)) {
            advance();
            if (at(TokenType::Newline) || at(TokenType::EndOfFile) || at(TokenType::Dedent)) break;
            auto elem = parseExpr();
            if (at(TokenType::Comma)) rejectInlineFnPack(*elem);  // a mid-pack inline fn is just as ambiguous
            tup->elems.push_back(std::move(elem));
        }
        return tup;
    }

    // Reject a comma-pack whose element is a bare inline-bodied function literal (see parseValueSeq).
    void rejectInlineFnPack(const ast::Expr& e) {
        if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e); fn && fn->inlineBody)
            throw KiritoError("an inline function cannot be comma-packed here (its trailing comma is "
                              "ambiguous); use an indented block body, or wrap it in a List with [ ]",
                              e.span);
    }

    ast::Block parseBlock() {
        expect(TokenType::Colon, "':'");
        return parseIndentedSuite();
    }

    // The ':'-introduced indented block, after the colon has been consumed.
    ast::Block parseIndentedSuite() {
        // Nested blocks (if/while/for/...) recurse through here; share the expression depth budget so a
        // pathologically deep indentation pyramid throws instead of overflowing the stack (parsing AND
        // the recursive AST teardown that follows).
        DepthGuard g(exprDepth_, peek().span);
        expect(TokenType::Newline, "a newline before an indented block");
        expect(TokenType::Indent, "an indented block");
        ast::Block body;
        while (!at(TokenType::Dedent) && !at(TokenType::EndOfFile)) {
            if (at(TokenType::Newline)) { advance(); continue; }
            body.push_back(parseStatement());
        }
        expect(TokenType::Dedent, "a dedent to close the block");
        blockJustClosed_ = true;  // the dedent self-terminates an enclosing simple statement
        return body;
    }

    ast::StmtPtr parseIf() {
        auto node = std::make_unique<ast::IfStmt>();
        node->span = advance().span;  // 'if'
        {
            auto cond = parseExpr();
            auto body = parseBlock();
            node->branches.emplace_back(std::move(cond), std::move(body));
        }
        while (at(TokenType::KwElif)) {
            advance();
            auto cond = parseExpr();
            auto body = parseBlock();
            node->branches.emplace_back(std::move(cond), std::move(body));
        }
        if (at(TokenType::KwElse)) {
            advance();
            node->orelse = parseBlock();
        }
        return node;
    }

    ast::StmtPtr parseWhile() {
        auto node = std::make_unique<ast::WhileStmt>();
        node->span = advance().span;  // 'while'
        node->cond = parseExpr();
        ++loopDepth_;
        node->body = parseBlock();
        --loopDepth_;
        return node;
    }

    ast::StmtPtr parseFor() {
        auto node = std::make_unique<ast::ForStmt>();
        node->span = advance().span;  // 'for'
        parseTargetNameList(node->vars, node->starIndex, "a loop variable");
        expect(TokenType::KwIn, "'in'");
        node->iterable = parseExpr();
        ++loopDepth_;
        node->body = parseBlock();
        --loopDepth_;
        return node;
    }

    // Bound recursion depth so pathological source (deeply nested parens/lists/calls/operators)
    // throws a clean error instead of overflowing the native stack while parsing. RAII so every
    // exit path (including thrown errors) decrements.
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& depth, SourceSpan span) : d(depth) {
            if (++d > kMaxParseDepth) throw KiritoError("expression nested too deeply", span);
        }
        ~DepthGuard() { --d; }
    };
    // The operator/postfix loops below build LEFT-deep trees iteratively (a[0][1]..., 1+1+1+...), so a
    // single long chain never trips DepthGuard's recursion bound yet still produces a tree that deep —
    // which then overflows the native stack when the unique_ptr children cascade at teardown. ChainDepth
    // makes each chain link count toward the same budget (and nested chains compound through it), so a
    // pathological chain throws "expression nested too deeply" at parse time instead. The whole
    // accumulated count is released at scope exit (RAII, exception-safe).
    struct ChainDepth {
        int& d; int n = 0;
        explicit ChainDepth(int& depth) : d(depth) {}
        ChainDepth(const ChainDepth&) = delete;
        ChainDepth& operator=(const ChainDepth&) = delete;
        void step(SourceSpan span) {
            if (++d > kMaxParseDepth) throw KiritoError("expression nested too deeply", span);
            ++n;
        }
        ~ChainDepth() { d -= n; }
    };
    // Recursive-descent nesting bound: throw a clean "nested too deeply" error before the native
    // stack overflows. Sanitizer builds have far larger frames, so the parser would overflow long
    // before 2000 — cap much shallower there (this still admits any non-pathological program).
#if defined(KIRITO_SANITIZER_BUILD)
    static constexpr int kMaxParseDepth = 250;
#else
    static constexpr int kMaxParseDepth = 2000;
#endif

    ast::ExprPtr parseExpr() {
        DepthGuard g(exprDepth_, peek().span);
        // Clear any stale "a block just closed" flag left by a PRECEDING nested suite (e.g. an if-body
        // before an `elif` condition, a case body before the next `case`, a try body before `catch`).
        // A block-bodied Function literal parsed *within* this expression re-sets it, so the
        // continuation guards still stop a next-line `(`/`,`/`.`/operator from gluing onto it (A02-1).
        blockJustClosed_ = false;
        return parseConditional();
    }

    // `then if cond else orelse` — the conditional expression, lowest precedence. The else-branch
    // recurses, so `a if c1 else b if c2 else c` is right-associative: a if c1 else (b if c2 else c).
    ast::ExprPtr parseConditional() {
        auto value = parseOr();
        // A block-bodied Function literal (or other indented suite) self-terminates its statement at
        // the closing dedent — just like endSimpleStatement. A following `if` therefore begins the
        // NEXT statement (e.g. the common `var f = Function(): ...` then `if argmain:` idiom) and must
        // not be swallowed as a ternary continuation of the function value.
        if (!at(TokenType::KwIf) || blockJustClosed_) return value;
        // The else-branch recurses here directly (not via parseExpr), so guard the chain depth to
        // keep a pathologically long `a if c else a if c else ...` from overflowing the native stack.
        DepthGuard g(exprDepth_, peek().span);
        auto node = std::make_unique<ast::ConditionalExpr>();
        node->span = advance().span;               // consume `if`
        node->then = std::move(value);
        node->cond = parseOr();
        expect(TokenType::KwElse, "'else' in a conditional expression");
        node->orelse = parseConditional();
        return node;
    }

    ast::ExprPtr parseOr() {
        ChainDepth cd(exprDepth_);
        auto left = parseAnd();
        while (at(TokenType::KwOr) && !blockJustClosed_) {
            cd.step(peek().span);
            SourceSpan span = advance().span;
            left = logical(std::move(left), /*isAnd=*/false, parseAnd(), span);
        }
        return left;
    }

    ast::ExprPtr parseAnd() {
        ChainDepth cd(exprDepth_);
        auto left = parseNot();
        while (at(TokenType::KwAnd) && !blockJustClosed_) {
            cd.step(peek().span);
            SourceSpan span = advance().span;
            left = logical(std::move(left), /*isAnd=*/true, parseNot(), span);
        }
        return left;
    }

    ast::ExprPtr parseNot() {
        if (at(TokenType::KwNot)) {
            DepthGuard g(exprDepth_, peek().span);  // bound deep `not not ... x` chains
            SourceSpan span = advance().span;
            auto node = std::make_unique<ast::UnaryExpr>();
            node->span = span;
            node->op = UnOp::Not;
            node->operand = parseNot();
            return node;
        }
        return parseComparison();
    }

    ast::ExprPtr parseComparison() {
        ChainDepth cd(exprDepth_);
        auto left = parseAdd();
        while (true) {
            if (blockJustClosed_) return left;  // a block-fn's dedent terminates the statement (A02-1)
            BinOp op;
            SourceSpan span;
            if (at(TokenType::KwIn)) {
                op = BinOp::In;
                span = advance().span;
            } else if (at(TokenType::KwNot) && peekAt(1).type == TokenType::KwIn) {
                op = BinOp::NotIn;
                span = advance().span;
                advance();
            } else {
                switch (peek().type) {
                    case TokenType::EqEq: { op = BinOp::Eq; } break;
                    case TokenType::NotEq: { op = BinOp::Ne; } break;
                    case TokenType::Lt: { op = BinOp::Lt; } break;
                    case TokenType::Le: { op = BinOp::Le; } break;
                    case TokenType::Gt: { op = BinOp::Gt; } break;
                    case TokenType::Ge: { op = BinOp::Ge; } break;
                    default: { return left; } break;
                }
                span = advance().span;
            }
            cd.step(span);
            left = binary(std::move(left), op, parseAdd(), span);
        }
    }

    ast::ExprPtr parseAdd() {
        ChainDepth cd(exprDepth_);
        auto left = parseMul();
        while ((at(TokenType::Plus) || at(TokenType::Minus)) && !blockJustClosed_) {
            BinOp op = at(TokenType::Plus) ? BinOp::Add : BinOp::Sub;
            SourceSpan span = advance().span;
            cd.step(span);
            left = binary(std::move(left), op, parseMul(), span);
        }
        return left;
    }

    ast::ExprPtr parseMul() {
        ChainDepth cd(exprDepth_);
        auto left = parseUnary();
        while ((at(TokenType::Star) || at(TokenType::Slash) ||
                at(TokenType::SlashSlash) || at(TokenType::Percent)) && !blockJustClosed_) {
            BinOp op = at(TokenType::Star)       ? BinOp::Mul
                     : at(TokenType::Slash)      ? BinOp::Div
                     : at(TokenType::SlashSlash) ? BinOp::FloorDiv
                                                 : BinOp::Mod;
            SourceSpan span = advance().span;
            cd.step(span);
            left = binary(std::move(left), op, parseUnary(), span);
        }
        return left;
    }

    ast::ExprPtr parseUnary() {
        if (at(TokenType::Minus)) {
            DepthGuard g(exprDepth_, peek().span);  // bound deep unary chains (---...---1)
            SourceSpan span = advance().span;
            auto node = std::make_unique<ast::UnaryExpr>();
            node->span = span;
            node->op = UnOp::Neg;
            node->operand = parseUnary();
            return node;
        }
        return parsePow();
    }

    ast::ExprPtr parsePow() {
        auto base = parsePostfix();
        if (at(TokenType::StarStar) && !blockJustClosed_) {
            DepthGuard g(exprDepth_, peek().span);  // ** is right-recursive (2**2**2**...): bound it
            SourceSpan span = advance().span;
            // Right-associative, and the exponent may itself be unary (2 ** -1).
            return binary(std::move(base), BinOp::Pow, parseUnary(), span);
        }
        return base;
    }

    // Postfix (call, index, member) binds tighter than **: f()**2 == (f())**2.
    ast::ExprPtr parsePostfix() {
        ChainDepth cd(exprDepth_);
        auto expr = parsePrimary();
        while (true) {
            // A block-bodied Function literal self-terminates at its dedent, so a leading `(`/`[`/`.` on
            // the next line begins a NEW statement — don't glue it into a call/index/member (A02-1).
            if (blockJustClosed_) break;
            if (!at(TokenType::LParen) && !at(TokenType::LBracket) && !at(TokenType::Dot)) break;
            cd.step(peek().span);   // each call/index/member link deepens the tree by one
            if (at(TokenType::LParen)) {
                SourceSpan span = advance().span;
                auto call = std::make_unique<ast::CallExpr>();
                call->span = span;
                call->callee = std::move(expr);
                if (!at(TokenType::RParen)) {
                    call->args.push_back(parseArg());
                    while (at(TokenType::Comma)) { advance(); call->args.push_back(parseArg()); }
                }
                expect(TokenType::RParen, "')' to close the call");
                expr = std::move(call);
            } else if (at(TokenType::LBracket)) {
                SourceSpan span = advance().span;
                ast::ExprPtr start, stop, step;
                bool isSlice = false;
                std::vector<ast::ExprPtr> extra;  // extra comma-separated indices: obj[a, b, c]
                if (!at(TokenType::Colon) && !at(TokenType::RBracket)) start = parseExpr();
                if (at(TokenType::Colon)) {
                    isSlice = true;
                    advance();
                    if (!at(TokenType::Colon) && !at(TokenType::RBracket)) stop = parseExpr();
                    if (at(TokenType::Colon)) {
                        advance();
                        if (!at(TokenType::RBracket)) step = parseExpr();
                    }
                } else {
                    // Comma-separated extra indices: obj[a, b, c]
                    while (at(TokenType::Comma)) {
                        advance();
                        extra.push_back(parseExpr());
                    }
                }
                expect(TokenType::RBracket, "']' to close the index");
                if (isSlice) {
                    auto node = std::make_unique<ast::SliceExpr>();
                    node->span = span;
                    node->object = std::move(expr);
                    node->start = std::move(start);
                    node->stop = std::move(stop);
                    node->step = std::move(step);
                    expr = std::move(node);
                } else {
                    // An empty subscript `obj[]` is not a slice and has no index — reject it here
                    // rather than building an IndexExpr with a null index (which the resolver /
                    // analyzer / compiler would dereference and crash on).
                    if (!start) throw KiritoError("expected an index expression inside '[ ]'", span);
                    auto node = std::make_unique<ast::IndexExpr>();
                    node->span = span;
                    node->object = std::move(expr);
                    node->indices.push_back(std::move(start));
                    for (auto& ex : extra) node->indices.push_back(std::move(ex));
                    expr = std::move(node);
                }
            } else if (at(TokenType::Dot)) {
                SourceSpan span = advance().span;
                auto node = std::make_unique<ast::MemberExpr>();
                node->span = span;
                node->object = std::move(expr);
                node->name = parseMemberName();
                expr = std::move(node);
            } else {
                break;
            }
        }
        return expr;
    }

    // One call argument: `name = value` (named) or a bare expression (positional).
    ast::Arg parseArg() {
        ast::Arg arg;
        if (peek().type == TokenType::Identifier && peekAt(1).type == TokenType::Assign) {
            arg.name = advance().text;  // the name
            advance();                  // '='
        }
        arg.value = parseExpr();
        return arg;
    }

    // One parameter: name [: Type] [= default].
    // A member name after '.': normally an identifier, but a keyword spelling is also valid as an
    // attribute (e.g. set.discard(), obj.type) — the token still carries its text. This keeps
    // language keywords from colliding with method/attribute names.
    std::string parseMemberName() {
        const Token& t = peek();
        // A member name is an identifier or a keyword used as an attribute (set.discard(), obj.type) —
        // the keyword token carries its spelling as text. A String/Number *literal* token also carries
        // text (so `obj."foo"` must NOT sneak through as `obj.foo`), so exclude the literal token types
        // explicitly instead of sniffing text[0].
        bool isLiteral = t.type == TokenType::String || t.type == TokenType::FString ||
                         t.type == TokenType::Integer || t.type == TokenType::Float;
        if (!isLiteral && !t.text.empty()) {
            char c0 = t.text[0];
            if (t.type == TokenType::Identifier || c0 == '_' || std::isalpha(static_cast<unsigned char>(c0)))
                return advance().text;
        }
        throw KiritoError("expected a member name after '.'", peek().span);
    }

    // A type annotation is a type/class name: an identifier, or the built-in `None` type.
    std::string parseTypeName(const char* what) {
        if (at(TokenType::KwNone)) { advance(); return "None"; }
        return expect(TokenType::Identifier, what).text;
    }

    ast::Param parseParam() {
        ast::Param p;
        p.name = expect(TokenType::Identifier, "a parameter name").text;
        if (at(TokenType::Colon)) {
            advance();
            p.annotation = parseTypeName("a type name after ':'");
        }
        if (at(TokenType::Assign)) {
            advance();
            p.defaultValue = parseExpr();
        }
        return p;
    }

    ast::ExprPtr parseFunction() {
        auto node = std::make_unique<ast::FunctionExpr>();
        std::size_t startTok = pos_;                    // the 'Function' keyword — start of the literal
        node->span = advance().span;  // 'Function'
        expect(TokenType::LParen, "'(' after Function");
        bool seenDefault = false;
        if (!at(TokenType::RParen)) {
            while (true) {
                auto pspan = peek().span;                 // the parameter name, for a precise diagnostic
                ast::Param p = parseParam();
                // A duplicate parameter name is a hard parse error (like Python's SyntaxError), not a
                // warn-and-run: a duplicate would desync the resolver's slot layout from the runtime
                // frame layout (wrong-slot reads / an assertion abort), so reject it up front.
                for (const auto& q : node->params)
                    if (q.name == p.name)
                        throw KiritoError("duplicate parameter name '" + p.name + "'", pspan);
                if (p.defaultValue) seenDefault = true;
                else if (seenDefault)
                    throw KiritoError("non-default parameter '" + p.name +
                                      "' follows a default parameter", peek().span);
                node->params.push_back(std::move(p));
                if (!at(TokenType::Comma)) break;
                advance();
            }
        }
        expect(TokenType::RParen, "')' after parameters");
        if (at(TokenType::Arrow)) {
            advance();
            node->returnAnnotation = parseTypeName("a return type after '->'");
        }
        expect(TokenType::Colon, "':' after Function parameters");
        // A function body is its own break/continue/return context: a loop outside the function
        // does not extend into it, and return becomes valid.
        int savedLoop = loopDepth_;
        loopDepth_ = 0;
        ++funcDepth_;
        if (at(TokenType::Newline)) {
            node->body = parseIndentedSuite();  // block body
        } else {
            // Inline body: one statement on the same line, e.g. Function(x): return x * x. It is a
            // RESTRICTED single statement (see parseInlineStatement) — return/pass/todo/throw/discard/
            // assert, a bare expression, or an assignment; a `var`/`if`/loop needs a block body.
            node->inlineBody = true;
            node->body.push_back(parseInlineStatement());
        }
        --funcDepth_;
        loopDepth_ = savedLoop;
        node->source = captureSource(startTok);  // verbatim text, for serialization
        // An INLINE body may have been written across physical lines because it sits inside a
        // (`/`[`/`{` — where the lexer suppresses newlines (line continuation). captureSource slices
        // that text WITHOUT the enclosing bracket, so serde re-parsing it standalone (at paren depth 0)
        // would see the now-significant newline and fail (A01-1). Wrap an inline literal's source in
        // parens to restore exactly the newline suppression it was parsed under. A block body must NOT
        // be wrapped (its Indents would be suppressed) — and a block body can never appear inside
        // brackets anyway, so an inline literal is the only across-lines case.
        if (node->inlineBody) node->source = "(" + node->source + ")";
        return node;
    }

    // A single statement for an inline function body. Unlike parseStatement it does not consume a
    // statement terminator, so the enclosing context (e.g. a call's argument list) continues.
    ast::StmtPtr parseInlineStatement() {
        switch (peek().type) {
            case TokenType::KwReturn: {
                auto node = std::make_unique<ast::ReturnStmt>();
                node->span = advance().span;
                if (!at(TokenType::Newline) && !at(TokenType::EndOfFile) &&
                    !at(TokenType::Comma) && !at(TokenType::RParen) && !at(TokenType::RBracket))
                    node->value = parseExpr();
                return node;
            } break;
            case TokenType::KwPass: {
                auto node = std::make_unique<ast::PassStmt>();
                node->span = advance().span;
                return node;
            } break;
            case TokenType::KwTodo: {
                auto node = std::make_unique<ast::TodoStmt>();
                node->span = advance().span;
                if (at(TokenType::String)) node->message = advance().text;  // optional reminder
                return node;
            } break;
            case TokenType::KwThrow: {
                auto node = std::make_unique<ast::ThrowStmt>();
                node->span = advance().span;
                node->value = parseExpr();
                return node;
            } break;
            case TokenType::KwDiscard: {   // `discard EXPR` — the analyzer recommends it inside inline bodies (A01-4)
                auto node = std::make_unique<ast::DiscardStmt>();
                node->span = advance().span;
                node->expr = parseExpr();
                return node;
            } break;
            case TokenType::KwAssert: {
                auto node = std::make_unique<ast::AssertStmt>();
                node->span = advance().span;
                node->cond = parseExpr();
                if (at(TokenType::Comma)) { advance(); node->message = parseExpr(); }
                return node;
            } break;
            default: {
                SourceSpan span = peek().span;
                auto expr = parseExpr();
                if (at(TokenType::Assign)) {
                    advance();
                    auto node = std::make_unique<ast::AssignStmt>();
                    node->span = span;
                    node->target = std::move(expr);
                    node->value = parseExpr();
                    return node;
                }
                auto node = std::make_unique<ast::ExprStmt>();
                node->span = span;
                node->expr = std::move(expr);
                return node;
            } break;
        }
    }

    ast::ExprPtr parsePrimary() {
        const Token& t = peek();
        switch (t.type) {
            case TokenType::Integer: {
                advance();
                return literal(intLiteral(t.text), t.span);
            } break;
            case TokenType::Float: {
                advance();
                // parseDouble (not std::stod) so a subnormal literal like 5e-324 yields the value
                // instead of crashing with an uncaught std::out_of_range. A literal too large for
                // double overflows to +inf (consistent with runtime
                // float overflow) rather than throwing — never crash the parser on a huge literal.
                double d;
                try {
                    d = parseDouble(t.text);
                } catch (const std::out_of_range&) {
                    d = HUGE_VAL;   // the literal token is unsigned; a leading '-' is a separate unary op
                }
                return literal(d, t.span);
            } break;
            case TokenType::String: {
                advance();
                return literal(t.text, t.span);
            } break;
            case TokenType::FString: {
                advance();
                return parseFString(t);
            } break;
            case TokenType::KwTrue: { advance(); return literal(true, t.span); } break;
            case TokenType::KwFalse: { advance(); return literal(false, t.span); } break;
            case TokenType::KwNone: { advance(); return literal(std::monostate{}, t.span); } break;
            case TokenType::KwFunction: { return parseFunction(); } break;
            case TokenType::Identifier: {
                advance();
                auto node = std::make_unique<ast::NameExpr>();
                node->span = t.span;
                node->name = t.text;
                return node;
            } break;
            case TokenType::LParen: {
                advance();
                auto e = parseExpr();
                expect(TokenType::RParen, "')'");
                return e;
            } break;
            case TokenType::LBracket: { return parseListLiteral(); } break;
            case TokenType::LBrace: { return parseBraceLiteral(); } break;
            default: {
                throw KiritoError("expected an expression", t.span);
            } break;
        }
    }

    ast::ExprPtr parseListLiteral() {
        auto node = std::make_unique<ast::ListLiteral>();
        node->span = advance().span;  // '['
        while (!at(TokenType::RBracket)) {
            node->elems.push_back(parseExpr());
            if (!at(TokenType::Comma)) break;
            advance();
        }
        expect(TokenType::RBracket, "']' to close the list");
        return node;
    }

    // `{}` and `{k: v, ...}` are Dicts; `{a, b, ...}` is a Set.
    ast::ExprPtr parseBraceLiteral() {
        SourceSpan span = advance().span;  // '{'
        if (at(TokenType::RBrace)) {
            advance();
            auto dict = std::make_unique<ast::DictLiteral>();
            dict->span = span;
            return dict;
        }
        auto first = parseExpr();
        if (at(TokenType::Colon)) {
            auto dict = std::make_unique<ast::DictLiteral>();
            dict->span = span;
            advance();
            dict->entries.emplace_back(std::move(first), parseExpr());
            while (at(TokenType::Comma)) {
                advance();
                if (at(TokenType::RBrace)) break;
                auto key = parseExpr();
                expect(TokenType::Colon, "':' in dict entry");
                dict->entries.emplace_back(std::move(key), parseExpr());
            }
            expect(TokenType::RBrace, "'}' to close the dict");
            return dict;
        }
        auto set = std::make_unique<ast::SetLiteral>();
        set->span = span;
        set->elems.push_back(std::move(first));
        while (at(TokenType::Comma)) {
            advance();
            if (at(TokenType::RBrace)) break;
            set->elems.push_back(parseExpr());
        }
        expect(TokenType::RBrace, "'}' to close the set");
        return set;
    }

    // Parse one embedded expression out of an f-string's {...}.
    ast::ExprPtr parseEmbedded(const std::string& rawCode, SourceSpan span) {
        // Trim surrounding whitespace: `f"{ x }"` is valid, and a leading space would otherwise make
        // the (indentation-sensitive) sub-lexer emit an INDENT and fail.
        std::size_t b = rawCode.find_first_not_of(" \t");
        std::size_t e = rawCode.find_last_not_of(" \t");
        std::string code = (b == std::string::npos) ? std::string() : rawCode.substr(b, e - b + 1);
        if (code.empty()) throw KiritoError("f-string '{...}' must contain a single expression", span);
        // The embedded expression is lexed/parsed by a sub Lexer+Parser SEEDED at the f-string token's
        // line/col, so every AST node it produces carries a span absolute to the source file (not line 1)
        // — runtime/resolver errors inside an f-string then report the real file location (A02-2). Parse
        // errors are also re-anchored to the token span for the same reason.
        ast::Program prog;
        try {
            Lexer lex(code, span.line, span.col);
            Parser sub(lex.tokenize(), exprDepth_ + 1);  // carry nesting so deep f-string nesting is bounded
            prog = sub.parseProgram();
        } catch (const KiritoError& err) {
            throw KiritoError(err.what(), span);
        }
        if (prog.stmts.size() != 1)
            throw KiritoError("f-string '{...}' must contain a single expression", span);
        auto* es = dynamic_cast<ast::ExprStmt*>(prog.stmts[0].get());
        if (!es) throw KiritoError("f-string '{...}' must contain an expression", span);
        return std::move(es->expr);
    }

    ast::ExprPtr parseFString(const Token& t) {
        auto node = std::make_unique<ast::FStringExpr>();
        node->span = t.span;
        const std::string& raw = t.text;
        std::string lit;
        auto flush = [&] {
            if (!lit.empty()) {
                ast::FStringExpr::Part p;
                p.literal = lit;
                node->parts.push_back(std::move(p));
                lit.clear();
            }
        };
        for (std::size_t i = 0; i < raw.size();) {
            char c = raw[i];
            if (c == '{') {
                if (i + 1 < raw.size() && raw[i + 1] == '{') { lit += '{'; i += 2; continue; }
                flush();
                // Find the matching `}` that closes this `{`. The scan must be QUOTE-AWARE: a brace
                // inside an embedded string literal (`f"{d['}']}"`) is data, not structure — counting
                // it would truncate the expression. Track a string state (open-quote char + escape) so
                // braces/colons inside `'...'`/`"..."` are skipped.
                std::size_t depth = 1, j = i + 1;
                char quote = 0;
                bool esc = false;
                for (; j < raw.size(); ++j) {
                    char cj = raw[j];
                    if (quote) {
                        if (esc) esc = false;
                        else if (cj == '\\') esc = true;
                        else if (cj == quote) quote = 0;
                        continue;
                    }
                    if (cj == '\'' || cj == '"') quote = cj;
                    else if (cj == '{') ++depth;
                    else if (cj == '}' && --depth == 0) break;
                }
                if (j >= raw.size() || depth != 0) throw KiritoError("unmatched '{' in f-string", t.span);
                ast::FStringExpr::Part p;
                p.isExpr = true;
                std::string inner = raw.substr(i + 1, j - (i + 1));
                // Split off an optional `:format-spec` at bracket depth 0 (so a `:` inside a slice
                // `x[1:2]` or a dict `{1:2}` is not mistaken for a spec separator) — and, likewise
                // quote-aware, so a `:` inside a string literal (`f"{'a:b'}"`) is not a spec separator.
                int bd = 0;
                char sq = 0;
                bool sesc = false;
                for (std::size_t k = 0; k < inner.size(); ++k) {
                    char ch = inner[k];
                    if (sq) {
                        if (sesc) sesc = false;
                        else if (ch == '\\') sesc = true;
                        else if (ch == sq) sq = 0;
                        continue;
                    }
                    if (ch == '\'' || ch == '"') sq = ch;
                    else if (ch == '(' || ch == '[' || ch == '{') ++bd;
                    else if (ch == ')' || ch == ']' || ch == '}') --bd;
                    else if (ch == ':' && bd == 0) { p.spec = inner.substr(k + 1); inner.resize(k); break; }
                }
                p.expr = parseEmbedded(inner, t.span);
                node->parts.push_back(std::move(p));
                i = j + 1;
            } else if (c == '}') {
                if (i + 1 < raw.size() && raw[i + 1] == '}') { lit += '}'; i += 2; continue; }
                throw KiritoError("single '}' in f-string", t.span);
            } else if (c == '\\' && !t.raw && i + 1 < raw.size()) {
                // Route through the SAME cooked-escape decoder the lexer uses (common.hpp), so an f-
                // string's `\xHH` emits the U+00HH code point (not a raw byte) and an unknown escape
                // throws a lex error — identical to a plain string. Previously these diverged
                // (f"\xff" != "\xff"; f"\q" silently became "q").
                std::string err;
                std::size_t consumed = decodeCookedEscape(raw, i, lit, err);
                if (consumed == 0) throw KiritoError(err, t.span);
                i += consumed;
            } else {
                lit += c;
                ++i;
            }
        }
        flush();
        return node;
    }

    // Decode an Integer literal's text into an int64. Decimal, or a base prefix (0x / 0b / 0o,
    // validated by the lexer), are accumulated the same way in unsigned 64-bit space, so an
    // over-wide literal wraps two's-complement (0xFFFFFFFFFFFFFFFF == -1, and 9223372036854775808
    // == INT64_MIN — which makes `-9223372036854775808` work) instead of overflowing. Crucially this
    // never throws: a giant decimal like 99999999999999999999 wraps rather than aborting (std::stoll
    // would throw std::out_of_range).
    static int64_t intLiteral(const std::string& s) {
        int base = 10;
        std::size_t i = 0;
        if (s.size() >= 2 && s[0] == '0') {
            char b = s[1];
            if (b == 'x' || b == 'X') { base = 16; i = 2; }
            else if (b == 'b' || b == 'B') { base = 2; i = 2; }
            else if (b == 'o' || b == 'O') { base = 8; i = 2; }
        }
        uint64_t v = 0;
        for (; i < s.size(); ++i) {
            char c = s[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
        }
        return static_cast<int64_t>(v);
    }

    template <typename T>
    static ast::ExprPtr literal(T value, SourceSpan span) {
        auto node = std::make_unique<ast::LiteralExpr>();
        node->span = span;
        node->value = std::move(value);
        return node;
    }

    static ast::ExprPtr binary(ast::ExprPtr lhs, BinOp op, ast::ExprPtr rhs, SourceSpan span) {
        auto node = std::make_unique<ast::BinaryExpr>();
        node->span = span;
        node->op = op;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }

    static ast::ExprPtr logical(ast::ExprPtr lhs, bool isAnd, ast::ExprPtr rhs, SourceSpan span) {
        auto node = std::make_unique<ast::LogicalExpr>();
        node->span = span;
        node->isAnd = isAnd;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }

    // Source-capture support (empty when the parser was constructed without source): the verbatim
    // newline-normalized source plus the byte offset of each line start, so a token's absolute
    // (line, col) maps to a byte offset. col counts BYTES (the lexer advances one column per byte), so
    // byteOf({line,col}) = lineStart_[line-1] + col-1. Used to slice a Function/class construct's exact
    // source text between the byte offset of its first token and the token that follows it.
    std::string source_;
    std::vector<std::size_t> lineStart_;
    void buildLineIndex() {
        lineStart_.push_back(0);
        for (std::size_t i = 0; i < source_.size(); ++i)
            if (source_[i] == '\n') lineStart_.push_back(i + 1);
    }
    std::size_t byteOf(const SourceSpan& s) const {
        if (lineStart_.empty() || s.line == 0) return 0;
        std::size_t li = s.line - 1;
        if (li >= lineStart_.size()) return source_.size();
        std::size_t off = lineStart_[li] + (s.col == 0 ? 0 : s.col - 1);
        return off > source_.size() ? source_.size() : off;
    }
    // Byte offset just past the last content token consumed — the end of a construct whose first token
    // is `toks_[startTok]` and which has just been fully parsed. The trailing Newline/Indent/Dedent
    // structure tokens are skipped because they sit at, or past, any comments and blank lines that
    // follow the body: comments emit no token, so anchoring on the next real token would sweep them in.
    std::size_t contentEnd(std::size_t startTok) const {
        std::size_t i = pos_;
        while (i > startTok + 1 && isStructural(toks_[i - 1].type)) --i;
        const Token& last = toks_[i - 1];
        return byteOf(last.span) + last.span.length;
    }
    static bool isStructural(TokenType t) {
        return t == TokenType::Newline || t == TokenType::Indent || t == TokenType::Dedent ||
               t == TokenType::EndOfFile;
    }

    // The verbatim source of the just-parsed construct whose first token is `toks_[startTok]`, right-
    // trimmed of trailing whitespace. Empty when no source was supplied (an f-string's re-parsed
    // expression) — which is what makes a Function defined there unserializable.
    std::string captureSource(std::size_t startTok) const {
        if (source_.empty()) return {};
        std::size_t a = byteOf(toks_[startTok].span);
        std::size_t b = std::min(contentEnd(startTok), source_.size());
        if (b <= a) return {};
        std::string text = source_.substr(a, b - a);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                 text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    bool blockJustClosed_ = false;
    int loopDepth_ = 0;   // > 0 inside a while/for body (for break/continue validity)
    int funcDepth_ = 0;   // > 0 inside a function body (for return validity)
    int exprDepth_ = 0;   // current expression nesting (bounded by DepthGuard, anti-stack-overflow)
};

}  // namespace kirito

#endif
