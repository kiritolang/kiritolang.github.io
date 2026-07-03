#ifndef KIRITO_STDLIB_SYS_HPP
#define KIRITO_STDLIB_SYS_HPP

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>

#include "builtins.hpp"
#include "collections.hpp"
#include "native.hpp"
#include "proc_compat.hpp"
#include "version.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
extern "C" char** environ;
#else
#  include <unistd.h>
extern "C" char** environ;
#endif

namespace kirito {

// Run an external program (argv) with optional cwd / stdin / timeout, returning a Dict
// {code, stdout, stderr}. Shared by sys.createprocess (argv directly) and sys.shell (argv wrapping a
// shell). A spawn failure or timeout becomes a clean catchable KiritoError.
inline Handle runExternalProcess(KiritoVM& vm, const std::vector<std::string>& argv,
                                 Value cwdV, Value inputV, Value timeoutV) {
    std::string cwd = cwdV.isNone() ? std::string() : cwdV.asStringRef("cwd");
    std::string input = inputV.isNone() ? std::string() : inputV.asStringRef("input");
    double timeout = timeoutV.isNone() ? 0.0 : timeoutV.asFloat("timeout");
    proccompat::ProcResult r;
    try {
        r = proccompat::run(argv, cwd, input, timeout);
    } catch (const proccompat::ProcError& e) {
        throw KiritoError(std::string(e.what()));
    }
    Dict d(vm);
    d.set("code", vm.makeInt(r.code));
    d.set("stdout", Value(vm, r.out));
    d.set("stderr", Value(vm, r.err));
    return d;
}

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// The `sys` module: process environment and platform facilities. Environment access is portable
// (getenv plus a platform setter); no global Kirito state — everything goes through the VM.
class SysModule : public NativeModule {
public:
    std::string name() const override { return "sys"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();

        // platform name, as a value.
#if defined(_WIN32)
        m.value("platform", vm.makeString("windows"));
#elif defined(__APPLE__)
        m.value("platform", vm.makeString("darwin"));
#else
        m.value("platform", vm.makeString("linux"));
#endif

        // CPU architecture, normalised to the names used in release-asset filenames (ki-<os>-<arch>).
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
        m.value("arch", vm.makeString("x64"));
#elif defined(__aarch64__) || defined(_M_ARM64)
        m.value("arch", vm.makeString("arm64"));
#elif defined(__i386__) || defined(_M_IX86)
        m.value("arch", vm.makeString("x86"));
#else
        m.value("arch", vm.makeString("unknown"));
#endif

        // The Kirito interpreter version (semver string). The absolute path of the running binary is
        // path.executable (a filesystem location, so it lives in `path`, not here).
        m.value("version", vm.makeString(kVersion));

        m.fn("getenv", {{"name", "String"}, {"default", "", vm.none()}}, "String", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // getenv(name[, default]) -> String, or default/None if unset.
            Args args(vm, a, "getenv");
            const char* v = std::getenv(args[0].asStringRef("getenv").c_str());
            if (v) return Value(vm, v);
            return args.opt(1, Value::None(vm));
        });

        m.fn("setenv", {{"name", "String"}, {"value", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "setenv");
            std::string name = args[0].asStringRef("setenv");
            std::string value = args[1].asStringRef("setenv");
#if defined(_WIN32)
            bool ok = ::SetEnvironmentVariableA(name.c_str(), value.c_str()) != 0;
            ::_putenv_s(name.c_str(), value.c_str());  // keep the CRT view (getenv) consistent too
#else
            bool ok = ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
            if (!ok) throw KiritoError("setenv failed for '" + name + "'");
            return Value::None(vm);
        });

        m.fn("unsetenv", {{"name", "String"}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            std::string name = Args(vm, a, "unsetenv")[0].asStringRef("unsetenv");
#if defined(_WIN32)
            ::SetEnvironmentVariableA(name.c_str(), nullptr);
            ::_putenv_s(name.c_str(), "");
#else
            ::unsetenv(name.c_str());
#endif
            return Value::None(vm);
        });

        // environ() -> Dict of all environment variables.
        m.fn("environ", {}, "Dict", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            Dict d(vm);
#if defined(_WIN32)
            LPCH block = ::GetEnvironmentStringsA();
            if (block) {
                for (LPCH p = block; *p; p += std::strlen(p) + 1) {
                    std::string entry(p);
                    std::size_t eq = entry.find('=');
                    if (eq != std::string::npos && eq > 0) d.set(entry.substr(0, eq), Value(vm, entry.substr(eq + 1)));
                }
                ::FreeEnvironmentStringsA(block);
            }
#else
            if (environ) {
                for (char** e = environ; *e; ++e) {
                    std::string entry(*e);
                    std::size_t eq = entry.find('=');
                    if (eq != std::string::npos) d.set(entry.substr(0, eq), Value(vm, entry.substr(eq + 1)));
                }
            }
#endif
            return d;
        });

        m.fn("exit", {{"code", "Integer", vm.makeInt(0)}}, "", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            // None/no-arg -> 0; an Integer -> that status; anything else is treated as
            // an error message printed to stderr with status 1 (rather than silently exiting 0, which
            // would mask a failure).
            if (a.empty()) std::exit(0);
            const Object& o = vm.arena().deref(a[0]);
            if (o.kind() == ValueKind::None) std::exit(0);
            if (o.kind() == ValueKind::Integer) std::exit(static_cast<int>(static_cast<const IntVal&>(o).value()));
            std::fprintf(stderr, "%s\n", vm.stringify(a[0]).c_str());
            std::exit(1);
            return Value::None(vm);  // unreachable
        });

        // traceback() -> the call chain of the MOST RECENT error this VM unwound, as a
        // String ("" if none yet). Useful inside a `catch` to see where the exception came from (which
        // file / function / line, innermost last). The traceback is VM-local — it reflects only errors
        // thrown in this VM, and is replaced by each new caught error.
        m.fn("traceback", {}, "String", [](KiritoVM& vm, std::span<const Handle>) -> Handle {
            return Value(vm, formatTraceback(vm.lastTraceback()));
        });

        // --- running external programs (NOT the `parallel` worker-VM model) -----------------------
        // createprocess(args, cwd=None, input="", timeout=None) -> {code, stdout, stderr}. Runs a
        // program DIRECTLY by its argument vector (args[0] is the program, found on PATH) — no shell,
        // so arguments are passed verbatim with no quoting/expansion/injection. stdout & stderr are
        // captured; `input` is fed on stdin; a positive `timeout` (seconds) kills it and throws.
        m.fn("createprocess",
             {{"args", "List"}, {"cwd", "", vm.none()}, {"input", "", vm.makeString("")}, {"timeout", "", vm.none()}},
             "Dict", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "createprocess");
            if (!args[0].isList())
                throw KiritoError("createprocess: args must be a List of Strings (the program and its arguments)");
            std::vector<std::string> argv;
            for (Value e : args[0].items()) argv.push_back(e.asStringRef("createprocess: each argument must be a String"));
            if (argv.empty()) throw KiritoError("createprocess: args must be a non-empty List (the program and its arguments)");
            return runExternalProcess(vm, argv, args.opt(1, Value::None(vm)), args.opt(2, Value(vm, "")), args.opt(3, Value::None(vm)));
        });

        // shell(command, cwd=None, input="", timeout=None) -> {code, stdout, stderr}. Runs `command`
        // through the system shell (/bin/sh -c on POSIX, cmd.exe /c on Windows), so shell features
        // (pipes, redirection, globbing, scripts) work. Capture the output with sys.shell(cmd)["stdout"].
        m.fn("shell",
             {{"command", "String"}, {"cwd", "", vm.none()}, {"input", "", vm.makeString("")}, {"timeout", "", vm.none()}},
             "Dict", [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            Args args(vm, a, "shell");
            std::string cmd = args[0].asStringRef("shell");
            std::vector<std::string> argv;
#if defined(_WIN32)
            argv = {"cmd.exe", "/c", cmd};
#else
            argv = {"/bin/sh", "-c", cmd};
#endif
            return runExternalProcess(vm, argv, args.opt(1, Value::None(vm)), args.opt(2, Value(vm, "")), args.opt(3, Value::None(vm)));
        });
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
