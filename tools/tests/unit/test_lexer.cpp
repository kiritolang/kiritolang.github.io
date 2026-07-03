#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

int main() {
    {
        Lexer lex("42");
        auto toks = lex.tokenize();
        // Integer, Newline, EndOfFile
        CHECK(toks.size() == 3);
        CHECK(toks[0].type == TokenType::Integer);
        CHECK(toks[0].text == "42");
        CHECK(toks[1].type == TokenType::Newline);
        CHECK(toks[2].type == TokenType::EndOfFile);
    }
    {
        Lexer lex("3.5 + 1");
        auto toks = lex.tokenize();
        CHECK(toks[0].type == TokenType::Float);
        CHECK(toks[0].text == "3.5");
        CHECK(toks[1].type == TokenType::Plus);
        CHECK(toks[2].type == TokenType::Integer);
    }
    {
        // operators, comments skipped, line/col tracked
        Lexer lex("1 // 2 ** 3  # comment\n");
        auto toks = lex.tokenize();
        CHECK(toks[0].type == TokenType::Integer);
        CHECK(toks[1].type == TokenType::SlashSlash);
        CHECK(toks[2].type == TokenType::Integer);
        CHECK(toks[3].type == TokenType::StarStar);
        CHECK(toks[4].type == TokenType::Integer);
        CHECK(toks[5].type == TokenType::Newline);
        CHECK(toks[0].span.line == 1);
        CHECK(toks[0].span.col == 1);
    }
    {
        // unexpected character reports a location
        Lexer lex("1 $ 2");
        CHECK_THROWS(lex.tokenize());
    }
    {
        // Universal newlines: CRLF (Windows / Windows->WSL copies) lexes identically to LF. A blank
        // CRLF line inside an indented block must still be recognized as blank, so the indent/dedent
        // stream is not corrupted (regression: this used to break class bodies with blank-line gaps).
        std::string crlf = "class C:\r\n    var a = 1\r\n\r\n    var b = 2\r\n";
        std::string lf   = "class C:\n    var a = 1\n\n    var b = 2\n";
        auto tc = Lexer(crlf).tokenize();
        auto tl = Lexer(lf).tokenize();
        CHECK(tc.size() == tl.size());
        bool sameTypes = true;
        for (std::size_t i = 0; i < tc.size() && i < tl.size(); ++i)
            if (tc[i].type != tl[i].type) sameTypes = false;
        CHECK(sameTypes);
        // and the blank line did not leak a stray Indent/Dedent: exactly one Indent opens the body
        std::size_t indents = 0, dedents = 0;
        for (const auto& t : tc) {
            if (t.type == TokenType::Indent) ++indents;
            if (t.type == TokenType::Dedent) ++dedents;
        }
        CHECK(indents == 1);
        CHECK(dedents == 1);
    }
    {
        // lone CR (classic-Mac line ending) is also treated as a newline
        Lexer lex("1\r2\r");
        auto toks = lex.tokenize();
        CHECK(toks[0].type == TokenType::Integer);
        CHECK(toks[1].type == TokenType::Newline);
        CHECK(toks[2].type == TokenType::Integer);
    }
    return RUN_TESTS();
}
