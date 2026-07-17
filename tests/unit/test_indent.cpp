#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

static int countOf(const std::vector<Token>& t, TokenType type) {
    int n = 0;
    for (const auto& tok : t) if (tok.type == type) ++n;
    return n;
}

int main() {
    {
        // if x:\n    y\nz\n  -> Indent after the block header, Dedent before z
        Lexer lex("if x:\n    y\nz\n");
        auto t = lex.tokenize();
        CHECK(t[0].type == TokenType::KwIf);
        CHECK(t[2].type == TokenType::Colon);
        CHECK(t[3].type == TokenType::Newline);
        CHECK(t[4].type == TokenType::Indent);
        CHECK(t[5].type == TokenType::Identifier);
        CHECK(t[5].text == "y");
        CHECK(t[6].type == TokenType::Newline);
        CHECK(t[7].type == TokenType::Dedent);
        CHECK(t[8].type == TokenType::Identifier);
        CHECK(t[8].text == "z");
    }
    {
        // nested: two Indents, and at EOF two Dedents are flushed
        Lexer lex("if a:\n    if b:\n        c\n");
        auto t = lex.tokenize();
        CHECK(countOf(t, TokenType::Indent) == 2);
        CHECK(countOf(t, TokenType::Dedent) == 2);
    }
    {
        // blank lines and comment-only lines produce no layout tokens
        Lexer lex("a\n\n   # comment\nb\n");
        auto t = lex.tokenize();
        CHECK(countOf(t, TokenType::Indent) == 0);
        CHECK(countOf(t, TokenType::Dedent) == 0);
    }
    {
        // a dedent that matches no enclosing level is an error
        Lexer lex("if x:\n    y\n  z\n");
        CHECK_THROWS(lex.tokenize());
    }
    {
        // ambiguous tabs-vs-spaces between sibling lines is an error: a tab (8 wide, 1 narrow)
        // and 8 spaces (8 wide, 8 narrow) look the same only under tab=8 -> rejected.
        Lexer lex("if x:\n\ty\n        z\n");
        CHECK_THROWS(lex.tokenize());
    }
    {
        // consistent indentation with tabs only is fine
        Lexer lex("if x:\n\ty\n\tz\n");
        auto t = lex.tokenize();
        CHECK(countOf(t, TokenType::Indent) == 1);
    }
    {
        // a lone space+tab indent (unambiguously deeper than 0) is allowed
        Lexer lex("if x:\n \ty\n");
        auto t = lex.tokenize();
        CHECK(countOf(t, TokenType::Indent) == 1);
    }
    return RUN_TESTS();
}
