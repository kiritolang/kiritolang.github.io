#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define ENV_SEP ";"
#else
#  define ENV_SEP ":"
#endif

#include "kirito.hpp"
#include "kirito/cli_paths.hpp"
#include "kirito/version.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <shellapi.h>
#endif

// To add a new module to the interpreter: #include its header and call vm.install<Module>().
// (The bundled stdlib is already registered by the VM; this is where a user wires in their own.)
//   #include "mymodule.hpp"
//   vm.install<MyModule>();

namespace {

#ifdef _WIN32
// Put the Windows console into UTF-8 mode for the program's lifetime, so Kirito's Unicode output
// (code-point strings, etc.) renders correctly instead of as mojibake, and restore it on exit. On
// non-Windows this is unnecessary (terminals are already UTF-8).
struct ConsoleUtf8 {
    UINT out_, in_;
    ConsoleUtf8() : out_(GetConsoleOutputCP()), in_(GetConsoleCP()) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
    ~ConsoleUtf8() { SetConsoleOutputCP(out_); SetConsoleCP(in_); }
};
#endif

void usage() {
    std::cout << "kirito (ki) - a high-level scripting language\n"
                 "usage: ki [options] [file.ki [args...]]\n"
                 "  with no file, starts an interactive REPL\n"
                 "options:\n"
                 "  --lib <dir>   add a directory to the module import path (repeatable)\n"
                 "  -w, --no-warn disable static-analysis warnings\n"
                 "  -v, --version print the Kirito version and exit\n"
                 "  -h, --help    show this help\n"
                 "environment:\n"
                 "  KIRITO_PATH   extra import directories (PATH-style, " ENV_SEP "-separated)\n"
                 "  packages installed by kpm under ~/.kirito/packages are importable directly\n";
}


int repl(kirito::KiritoVM& vm) {
    std::cout << "Kirito (ki) " << kirito::kVersion << " REPL - Ctrl-D to exit\n";
    std::string line;
    while (std::cout << ">>> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // Multiline blocks: if a line opens an indented suite (ends with ':', ignoring trailing
        // spaces and a trailing comment), keep reading with a continuation prompt until a blank
        // line, then evaluate the whole buffer. This lets `Function():`, `if`, `for`, `class`, etc.
        // be typed interactively instead of erroring on the missing indented block.
        auto opensBlock = [](const std::string& s) {
            std::size_t end = s.size();
            bool inStr = false;
            char q = 0;
            std::size_t comment = std::string::npos;
            for (std::size_t i = 0; i < s.size(); ++i) {  // find an unquoted '#'
                char c = s[i];
                if (inStr) { if (c == q && s[i - 1] != '\\') inStr = false; }
                else if (c == '"' || c == '\'') { inStr = true; q = c; }
                else if (c == '#') { comment = i; break; }
            }
            if (comment != std::string::npos) end = comment;
            while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
            return end > 0 && s[end - 1] == ':';
        };

        std::string source = line;
        if (opensBlock(line)) {
            std::string more;
            while (std::cout << "... " && std::getline(std::cin, more)) {
                if (more.empty()) break;  // a blank line ends the block
                source += "\n" + more;
            }
        }

        try {
            kirito::Handle result = vm.runRepl(source);
            if (vm.arena().deref(result).kind() != kirito::ValueKind::None)
                std::cout << vm.stringify(result) << "\n";
        } catch (const kirito::KiritoError& e) {
            std::cerr << "error: " << e.what() << " (line " << e.span.line << ":" << e.span.col << ")\n";
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";  // e.g. an uncaught std::bad_alloc from a native
        }
    }
    std::cout << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ConsoleUtf8 utf8Console;  // UTF-8 console for the duration of the program
    // Put the standard streams in BINARY mode: the C runtime otherwise translates '\n' <-> "\r\n" and
    // treats Ctrl-Z (0x1A) as EOF on stdin, which corrupts byte-exact I/O — notably a binary payload
    // piped through a child `ki` via sys.createprocess, and any io.read()/io.write() of raw bytes.
    // Kirito std I/O is then LF-clean and byte-transparent, matching POSIX; readLine() strips a
    // trailing '\r' so CRLF console/pipe input still yields clean lines (see stdlib_io.hpp).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    // Rebuild argv as UTF-8 from the wide command line. The CRT's narrow argv is decoded in the ANSI
    // codepage, which mangles any non-ASCII argument or path (e.g. a Unicode arg forwarded by
    // sys.createprocess, or a Unicode script path). Everything below treats argv as UTF-8.
    std::vector<std::string> wideArgs;
    std::vector<char*> wideArgv;
    if (int wn = 0; LPWSTR* wv = ::CommandLineToArgvW(::GetCommandLineW(), &wn)) {
        for (int i = 0; i < wn; ++i) {
            int blen = ::WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(blen > 0 ? static_cast<std::size_t>(blen - 1) : 0, '\0');
            if (blen > 1) ::WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, s.data(), blen, nullptr, nullptr);
            wideArgs.push_back(std::move(s));
        }
        ::LocalFree(wv);
        for (auto& s : wideArgs) wideArgv.push_back(s.data());
        argc = static_cast<int>(wideArgv.size());
        argv = wideArgv.data();
    }
#endif
    std::vector<std::string> libs;
    std::string file;
    std::vector<std::string> scriptArgs;
    bool warnings = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!file.empty()) {  // everything after the script name belongs to the script
            scriptArgs.push_back(arg);
        } else if (arg == "--lib") {
            if (++i >= argc) { std::cerr << "ki: --lib needs a directory\n"; return 2; }
            libs.emplace_back(argv[i]);
        } else if (arg.rfind("--lib=", 0) == 0) {
            libs.push_back(arg.substr(6));
        } else if (arg == "--no-warn" || arg == "-w") {
            warnings = false;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "ki (Kirito) " << kirito::kVersion << "\n";
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ki: unknown option '" << arg << "'\n";
            return 2;
        } else {
            file = arg;
        }
    }

    // The CLI runs every VM through a dispatcher, so the `parallel` module (spawn/Queue/...) is
    // available and worker VMs inherit the same import paths. A bare embedded KiritoVM has no
    // dispatcher and thus no `parallel` module — multiprocessing is a dispatcher-provided capability.
    kirito::KiritoDispatcher dispatcher;
    kirito::KiritoVM& vm = dispatcher.mainVM();
    dispatcher.addLibPath(".");  // current directory is always on the import path
    for (const auto& l : libs) dispatcher.addLibPath(l);

    // Environment-contributed import paths: KIRITO_PATH plus the per-user package directory that
    // `kpm` installs into (and each package sub-directory), so installed packages are importable
    // without an explicit --lib. See src/kirito/cli_paths.hpp.
#ifdef _WIN32
    const char pathSep = ';';
    const char* homeEnv = std::getenv("USERPROFILE");
#else
    const char pathSep = ':';
    const char* homeEnv = std::getenv("HOME");
#endif
    const char* kiritoPathEnv = std::getenv("KIRITO_PATH");
    for (const auto& d : kirito::environmentLibPaths(kiritoPathEnv ? kiritoPathEnv : "",
                                                     homeEnv ? homeEnv : "", pathSep))
        dispatcher.addLibPath(d);

    if (file.empty()) return repl(vm);

    // The script's own directory is also searched for sibling modules.
    std::error_code ec;
    std::filesystem::path scriptPath(file);
    if (scriptPath.has_parent_path()) dispatcher.addLibPath(scriptPath.parent_path().string());

    std::ifstream in(file);
    if (!in) { std::cerr << "ki: cannot open '" << file << "'\n"; return 1; }
    std::stringstream buffer;
    buffer << in.rdbuf();

    vm.setArgs(scriptArgs);  // bound as `arglist` in every module scope

    // Static analysis (non-fatal warnings) before execution: parse once, lint, print to stderr.
    if (warnings) {
        try {
            kirito::Parser parser(kirito::Lexer(buffer.str()).tokenize());
            kirito::ast::Program program = parser.parseProgram();
            kirito::Analyzer analyzer;
            for (const auto& w : kirito::formatWarnings(analyzer.analyze(program), file))
                std::cerr << w << "\n";
        } catch (const kirito::KiritoError&) {
            // A parse error here will be reported (with its span) by runSource below; skip linting.
        }
    }

    try {
        vm.runSource(buffer.str(), file);
    } catch (const kirito::KiritoError& e) {
        // Prefer the file the error was tagged with (e.g. an imported module), falling back to the
        // entry script when the error carries no location of its own. Print the call-chain
        // traceback first (when the error unwound through function frames), then the error line.
        const std::string& where = e.file.empty() ? file : e.file;
        std::cerr << kirito::formatTraceback(e.traceback);
        std::cerr << where << ":" << e.span.line << ":" << e.span.col << ": error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        // A native that throws a std::exception the user didn't catch (e.g. std::bad_alloc) must not
        // call std::terminate — report it and exit non-zero like any other error.
        std::cerr << file << ": error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
