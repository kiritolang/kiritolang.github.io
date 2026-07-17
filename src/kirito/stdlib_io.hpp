#ifndef KIRITO_STDLIB_IO_HPP
#define KIRITO_STDLIB_IO_HPP

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "bytes.hpp"
#include "collections.hpp"
#include "native.hpp"

namespace kirito {

// Raw bytes from a String or a Bytes argument (so binary write() / writelines() accept either).
inline const std::string& ioRawBytes(KiritoVM& vm, Handle h, const char* who) {
    Object& o = vm.arena().deref(h);
    if (o.kind() == ValueKind::String) return static_cast<StrVal&>(o).value();
    if (auto* b = dynamic_cast<BytesVal*>(&o)) return b->data;
    throw KiritoError(std::string(who) + " expects a String or Bytes");
}

// The native-binding idiom below re-uses `vm`/`self` as bound-method lambda parameters that
// intentionally shadow the enclosing getAttr/setup `vm`/`self` (same VM, by design). Silence
// -Wshadow for these mechanical bindings; it stays active in the evaluator/parser/lexer core.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#endif

// A uniform byte/character-stream interface implemented by every stream value (File, BytesIO, and
// the standard streams). It is what makes streams *interchangeable*: io.print/write/input/read act
// on whatever object is currently bound to io.stdout / io.stdin, dispatching through this interface
// — so redirecting output to a file (or a BytesIO, or back to the terminal) is just a rebinding.
// A value that is not an IoStream can still serve as stdout/stdin via its `write`/`readline` methods
// (duck typing); this interface is the fast, built-in path.
struct IoStream {
    virtual ~IoStream() = default;
    virtual void streamWrite(const std::string&) { throw KiritoError("stream is not writable"); }
    virtual std::string streamRead(std::optional<std::size_t>) { throw KiritoError("stream is not readable"); }
    virtual std::string streamReadLine() { throw KiritoError("stream is not readable"); }
    virtual void streamFlush() {}
};

// A file/stream object. Holds an fstream (closed automatically when the value is collected) and
// exposes read/readline/write/close plus enter/exit so it works as a `with` context manager.
class FileVal : public NativeClass<FileVal>, public IoStream {
public:
    static constexpr const char* kTypeName = "File";
    std::fstream stream;
    std::string path;
    bool binary = false;   // opened with a "b" mode: read() yields Bytes, write() accepts Bytes
    bool writable = false;
    bool readable = false;

    FileVal(const std::string& p, std::ios::openmode mode, bool bin = false)
        : path(p), binary(bin), writable((mode & std::ios::out) != 0), readable((mode & std::ios::in) != 0) {
        stream.open(p, mode);
    }

    // I/O on a closed or wrong-mode file throws (a silent no-op would lose data invisibly).
    void requireOpen() const {
        if (!stream.is_open()) throw KiritoError("I/O operation on closed file: " + path);
    }
    void requireWritable() const {
        requireOpen();
        if (!writable) throw KiritoError("file not open for writing: " + path);
    }
    void requireReadable() const {
        requireOpen();
        if (!readable) throw KiritoError("file not open for reading: " + path);
    }

    // Wrap raw bytes read from the stream as a Bytes (binary mode) or a String (text mode).
    Handle wrapRead(KiritoVM& vm, std::string s) const {
        if (binary) return vm.alloc(std::make_unique<BytesVal>(std::move(s)));
        return vm.makeString(std::move(s));
    }

    void streamWrite(const std::string& s) override {
        requireWritable();
        stream.clear();
        stream << s;
        if (stream.fail()) throw KiritoError("write failed: " + path);
    }
    std::string streamRead(std::optional<std::size_t> n) override {
        requireReadable();
        if (!n) { std::stringstream ss; ss << stream.rdbuf(); return ss.str(); }
        std::size_t want = *n;
        stream.clear();                               // a prior readline/iterate to EOF leaves eof/fail set
        std::streampos cur = stream.tellg();          // (making tellg()==-1); a huge read(n) must never try
        stream.seekg(0, std::ios::end);               // to allocate n bytes, so clamp `want` to what remains
        std::streampos endp = stream.tellg();         // — including when the cursor is unknown (cur<0) or
        stream.seekg(cur >= 0 ? cur : endp);          // seeked PAST end (cur>endp): both leave 0 available.
        if (endp < 0) {
            // Non-seekable (FIFO/device/proc): the size can't be measured, so read in bounded chunks
            // rather than pre-allocating `want` bytes — a huge read(n) would otherwise bad_alloc.
            stream.clear();                           // the seekg probes set failbit on a non-seekable stream
            std::string out;
            constexpr std::size_t kChunk = 65536;
            while (out.size() < want) {
                std::size_t take = std::min(kChunk, want - out.size());
                std::size_t base = out.size();
                out.resize(base + take);
                stream.read(out.data() + base, static_cast<std::streamsize>(take));
                std::size_t got = static_cast<std::size_t>(stream.gcount());
                out.resize(base + got);
                if (got < take) break;                // EOF / short read
            }
            return out;
        }
        std::size_t avail = (cur >= 0 && endp >= cur) ? static_cast<std::size_t>(endp - cur) : 0;
        want = std::min(want, avail);
        std::string buf(want, '\0');
        stream.read(buf.data(), static_cast<std::streamsize>(want));
        buf.resize(static_cast<std::size_t>(stream.gcount()));
        return buf;
    }
    std::string streamReadLine() override {
        requireReadable();
        std::string line;
        std::getline(stream, line);
        return line;
    }
    void streamFlush() override { stream.flush(); }

    // Iterating a file yields its remaining lines (so `for line in f:` works). Lazily — one line per
    // step — so a multi-GB log is never buffered whole (A10-5). The cursor keeps this FileVal rooted,
    // so `src` stays valid for the loop's duration.
    struct LineIter : LazyIterator {
        Handle src;
        explicit LineIter(Handle s) : src(s) {}
        std::optional<Handle> next(KiritoVM& vm) override {
            auto& f = static_cast<FileVal&>(vm.arena().deref(src));
            f.requireReadable();
            std::string line;
            if (!std::getline(f.stream, line)) return std::nullopt;
            return f.wrapRead(vm, std::move(line));
        }
    };
    std::unique_ptr<LazyIterator> lazyIterate(KiritoVM&, Handle self) override {
        requireReadable();
        return std::make_unique<LineIter>(self);
    }
    // Eager iterate() stays for direct consumers (List(f), sorted(f), set(f)); the for-loop prefers
    // lazyIterate above.
    std::optional<std::vector<Handle>> iterate(KiritoVM& vm) override {
        requireReadable();
        RootScope rs(vm);
        std::vector<Handle> lines;
        std::string line;
        while (std::getline(stream, line)) lines.push_back(rs.add(wrapRead(vm, line)));
        return lines;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"read(size) -> String", "readline() -> String", "readlines() -> List",
                "write(data)", "writelines(lines)", "seek(offset, whence) -> Integer", "tell() -> Integer",
                "flush()", "close()"};
    }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto file = [](KiritoVM& vm, Handle self) -> FileVal& {
            return static_cast<FileVal&>(vm.arena().deref(self));
        };
        if (name == "read")
            return bind("read", {"size"}, [self, file](KiritoVM& vm, std::span<const Handle> a) {
                std::optional<std::size_t> n;
                if (!a.empty()) {
                    int64_t want = Value(vm, a[0]).asInt("read");
                    if (want >= 0) n = static_cast<std::size_t>(want);
                }
                auto& f = file(vm, self);
                return f.wrapRead(vm, f.streamRead(n));      // Bytes if binary, else String
            });
        if (name == "readline")
            return bind("readline", {}, [self, file](KiritoVM& vm, std::span<const Handle>) {
                auto& f = file(vm, self);
                return f.wrapRead(vm, f.streamReadLine());
            });
        if (name == "write")
            return bind("write", {"data"}, [self, file](KiritoVM& vm, std::span<const Handle> a) {
                Args(vm, a, "write").require(1);
                file(vm, self).streamWrite(ioRawBytes(vm, a[0], "write"));  // String or Bytes
                return vm.none();
            });
        if (name == "close" || name == "_exit_")
            return bind("close", {}, [self, file](KiritoVM& vm, std::span<const Handle>) {
                file(vm, self).stream.close();
                return vm.none();
            });
        if (name == "readlines")
            return bind("readlines", {}, [self, file](KiritoVM& vm, std::span<const Handle>) -> Handle {
                RootScope rs(vm);
                auto& f = file(vm, self);
                f.requireReadable();
                auto list = std::make_unique<ListVal>();
                std::string line;
                while (std::getline(f.stream, line)) list->elems.push_back(rs.add(f.wrapRead(vm, line)));
                return vm.alloc(std::move(list));
            });
        if (name == "writelines")
            return bind("writelines", {"lines"}, [self, file](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "writelines").require(1);
                auto items = vm.arena().deref(a[0]).iterate(vm);
                if (!items) throw KiritoError("writelines: argument must be iterable");  // a write-only stream returns nullopt, not a throw
                auto& f = file(vm, self);
                for (Handle h : items.value()) f.streamWrite(ioRawBytes(vm, h, "writelines"));  // String or Bytes
                return vm.none();
            });
        if (name == "flush")
            return bind("flush", {}, [self, file](KiritoVM& vm, std::span<const Handle>) -> Handle {
                file(vm, self).stream.flush();
                return vm.none();
            });
        if (name == "tell")
            return bind("tell", {}, [self, file](KiritoVM& vm, std::span<const Handle>) -> Handle {
                auto& f = file(vm, self);
                f.requireOpen();
                if (!f.stream.good()) f.stream.clear();   // a prior read that hit EOF leaves eof/fail
                                                          // set; that's not an error for tell() — clear
                                                          // so tellg() reports the position, not -1.
                return vm.makeInt(static_cast<int64_t>(f.stream.tellg()));
            });
        if (name == "seek")
            return bind("seek", {"offset", "whence"}, [self, file](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                file(vm, self).requireOpen();
                if (a.empty()) throw KiritoError("seek expects an offset");
                int64_t off = Value(vm, a[0]).asInt("seek");
                int64_t whence = (a.size() > 1) ? Value(vm, a[1]).asInt("seek") : 0;  // 0=set, 1=cur, 2=end
                if (whence < 0 || whence > 2) throw KiritoError("seek: whence must be 0 (set), 1 (cur), or 2 (end)");
                auto& s = file(vm, self).stream;
                s.clear();
                // Resolve to an ABSOLUTE target first, then position both pointers there. An fstream's
                // get/put pointers share one position, so two *relative* seeks (seekg+seekp with cur)
                // would move it twice — compute the absolute offset once to avoid that double-count.
                int64_t target = off;
                if (whence == 1 || whence == 2) {
                    if (whence == 2) s.seekg(0, std::ios::end);
                    // base + off computed overflow-safe (a near-INT64_MAX off would otherwise wrap past
                    // the negative-target guard below).
                    if (__builtin_add_overflow(static_cast<int64_t>(s.tellg()), off, &target))
                        throw KiritoError("seek: resulting position is out of range");
                }
                // A negative absolute target would put the stream in a fail state and make tell() return
                // -1 silently; reject it (BytesIO::seek guards the same way).
                if (target < 0) throw KiritoError("seek: resulting position is negative");
                s.seekg(target, std::ios::beg);
                s.seekp(target, std::ios::beg);
                return vm.makeInt(static_cast<int64_t>(s.tellg()));    // returns the new position
            });
        if (name == "_enter_")
            return bind("_enter_", {}, [self](KiritoVM&, std::span<const Handle>) { return self; });
        return Object::getAttr(vm, self, name);
    }
};

// An in-memory binary buffer: an efficient growable byte buffer with a
// read/write cursor. write() appends/overwrites at the cursor; read() consumes from it; getvalue()
// returns the whole contents as a byte String. Useful as a sink for code that "writes a file"
// (e.g. encoding an image) without touching disk.
class BytesIO : public NativeClass<BytesIO>, public IoStream {
public:
    static constexpr const char* kTypeName = "BytesIO";
    static constexpr std::size_t kMaxBuf = 256ull * 1024 * 1024;  // bound the in-memory buffer (matches Bytes)
    std::string buf;
    std::size_t pos = 0;

    BytesIO() = default;
    explicit BytesIO(std::string initial) : buf(std::move(initial)) {}

    std::string str(StringifyCtx&) const override {
        return "BytesIO(" + std::to_string(buf.size()) + " bytes)";
    }

    void streamWrite(const std::string& data) override {
        if (pos > kMaxBuf || data.size() > kMaxBuf - pos)       // e.g. write after seek(9e18): overflow-safe
            throw KiritoError("BytesIO too large");
        if (pos + data.size() > buf.size()) buf.resize(pos + data.size());  // overwrite-at-cursor
        std::copy(data.begin(), data.end(), buf.begin() + static_cast<std::ptrdiff_t>(pos));
        pos += data.size();
    }
    std::string streamRead(std::optional<std::size_t> n) override {
        if (pos >= buf.size()) return "";           // cursor at/past end (e.g. after seek-beyond-end)
        std::size_t avail = buf.size() - pos;
        std::size_t take = n ? std::min(avail, *n) : avail;
        std::string out = buf.substr(pos, take);
        pos += out.size();
        return out;
    }
    std::string streamReadLine() override {
        if (pos >= buf.size()) return "";           // cursor at/past end: nothing to read
        std::size_t nl = buf.find('\n', pos);
        std::size_t end = (nl == std::string::npos) ? buf.size() : nl + 1;  // include the newline
        std::string out = buf.substr(pos, end - pos);
        pos = end;
        if (!out.empty() && out.back() == '\n') out.pop_back();  // strip trailing newline, like getline
        return out;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"read(size) -> String", "readline() -> String", "write(data) -> Integer",
                "getvalue() -> String", "seek(offset, whence) -> Integer", "tell() -> Integer",
                "size() -> Integer", "truncate() -> Integer", "flush()", "close()"};
    }

    std::optional<int64_t> length(KiritoVM&) override {   // len(b) -> number of buffered bytes
        return static_cast<int64_t>(buf.size());
    }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto io = [](KiritoVM& vm, Handle self) -> BytesIO& {
            return static_cast<BytesIO&>(vm.arena().deref(self));
        };
        if (name == "write")
            return bind("write", {"data"}, [self, io](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "BytesIO.write").require(1);                                // guard the positional fast path (a[0] OOB otherwise)
                const std::string& data = ioRawBytes(vm, a[0], "BytesIO.write");   // String or Bytes
                io(vm, self).streamWrite(data);
                return vm.makeInt(static_cast<int64_t>(data.size()));
            });
        if (name == "read")
            return bind("read", {"size"}, [self, io](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                std::optional<std::size_t> n;
                if (!a.empty() && vm.arena().deref(a[0]).kind() != ValueKind::None) {
                    int64_t want = Value(vm, a[0]).asInt("read");   // throw on a non-Integer size, not silently read-all
                    if (want >= 0) n = static_cast<std::size_t>(want);
                }
                return vm.makeString(io(vm, self).streamRead(n));
            });
        if (name == "readline")
            return bind("readline", {}, [self, io](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeString(io(vm, self).streamReadLine());
            });
        if (name == "flush")
            return bind("flush", {}, [](KiritoVM& vm, std::span<const Handle>) -> Handle { return vm.none(); });
        if (name == "getvalue")
            return bind("getvalue", {}, [self, io](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeString(io(vm, self).buf);
            });
        if (name == "tell")
            return bind("tell", {}, [self, io](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(io(vm, self).pos));
            });
        if (name == "seek")
            return bind("seek", {"offset", "whence"}, [self, io](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                auto& b = io(vm, self);
                if (a.empty()) throw KiritoError("seek expects an offset");
                int64_t off = Value(vm, a[0]).asInt("seek");
                int whence = a.size() > 1 ? static_cast<int>(Value(vm, a[1]).asInt("seek")) : 0;
                if (whence < 0 || whence > 2) throw KiritoError("seek: whence must be 0 (set), 1 (cur), or 2 (end)");
                int64_t base = whence == 1 ? static_cast<int64_t>(b.pos)
                             : whence == 2 ? static_cast<int64_t>(b.buf.size()) : 0;
                int64_t np;
                if (__builtin_add_overflow(base, off, &np)) throw KiritoError("seek: resulting position is out of range");
                if (np < 0) np = 0;  // BytesIO clamps a before-start seek to 0 (by design, unlike File.seek which throws)
                b.pos = static_cast<std::size_t>(np);
                return vm.makeInt(np);
            });
        if (name == "size" || name == "_len_")
            return bind("size", {}, [self, io](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeInt(static_cast<int64_t>(io(vm, self).buf.size()));
            });
        if (name == "truncate")
            return bind("truncate", {}, [self, io](KiritoVM& vm, std::span<const Handle>) -> Handle {
                auto& b = io(vm, self);
                // truncate() to the cursor. A truncate-that-EXTENDS (after a large seek) would
                // materialize the whole buffer, so it honours the same size cap as streamWrite —
                // else `b.seek(4e8); b.truncate()` silently allocated 400 MB past the write guard.
                if (b.pos > BytesIO::kMaxBuf) throw KiritoError("BytesIO too large");
                b.buf.resize(b.pos);
                return vm.makeInt(static_cast<int64_t>(b.buf.size()));
            });
        if (name == "_enter_")
            return bind("_enter_", {}, [self](KiritoVM&, std::span<const Handle>) { return self; });
        if (name == "_exit_" || name == "close")
            return bind("close", {}, [](KiritoVM& vm, std::span<const Handle>) { return vm.none(); });
        return Object::getAttr(vm, self, name);
    }
};

// A handle to one of the process's standard streams (stdout/stderr/stdin), wearing the same stream
// protocol as File and BytesIO so the three are interchangeable as io.print/input targets. Closing
// is a no-op — we never close the real cout/cin. Output streams reject reads and vice-versa.
class StdStream : public NativeClass<StdStream>, public IoStream {
public:
    static constexpr const char* kTypeName = "StdStream";
    enum class Dir { Out, Err, In };
    Dir dir;
    explicit StdStream(Dir d) : dir(d) {}

    std::string str(StringifyCtx&) const override {
        return dir == Dir::Out ? "<stdout>" : dir == Dir::Err ? "<stderr>" : "<stdin>";
    }
    void streamWrite(const std::string& s) override {
        if (dir == Dir::In) throw KiritoError("stdin is not writable");
        (dir == Dir::Err ? std::cerr : std::cout) << s;
    }
    void streamFlush() override {
        if (dir == Dir::Err) std::cerr.flush();
        else if (dir == Dir::Out) std::cout.flush();
    }
    std::string streamRead(std::optional<std::size_t> n) override {
        if (dir != Dir::In) throw KiritoError("a write stream is not readable");
        if (!n) { std::stringstream ss; ss << std::cin.rdbuf(); return ss.str(); }
        std::string out;                              // read in chunks: a huge n must not pre-allocate n bytes
        std::size_t remaining = *n;
        constexpr std::size_t kChunk = 1u << 16;
        std::array<char, kChunk> tmp{};
        while (remaining > 0 && std::cin) {
            std::cin.read(tmp.data(), static_cast<std::streamsize>(std::min(remaining, kChunk)));
            std::streamsize got = std::cin.gcount();
            if (got <= 0) break;
            out.append(tmp.data(), static_cast<std::size_t>(got));
            remaining -= static_cast<std::size_t>(got);
        }
        return out;
    }
    // On Windows the std streams are opened in binary mode (see main.cpp) for byte-exact read()/
    // write(), so a CRLF console/pipe line arrives with a trailing '\r' that getline keeps; strip it
    // here so line input stays clean (universal-newline). A no-op on POSIX, where there is no '\r'.
    static void stripCR(std::string& line) {
#ifdef _WIN32
        if (!line.empty() && line.back() == '\r') line.pop_back();
#else
        (void)line;
#endif
    }
    std::string streamReadLine() override {
        if (dir != Dir::In) throw KiritoError("a write stream is not readable");
        std::string line; std::getline(std::cin, line); stripCR(line); return line;
    }

    // `for line in io.stdin:` reads stdin line-by-line — LAZILY, one line per step, so interactive /
    // piped / `tail -f` input is processed as it arrives instead of buffered to EOF first (A10-5).
    struct StdinLineIter : LazyIterator {
        std::optional<Handle> next(KiritoVM& vm) override {
            std::string line;
            if (!std::getline(std::cin, line)) return std::nullopt;
            stripCR(line);
            return vm.makeString(std::move(line));
        }
    };
    std::unique_ptr<LazyIterator> lazyIterate(KiritoVM&, Handle) override {
        if (dir != Dir::In) return nullptr;   // a write stream isn't iterable -> eager path throws
        return std::make_unique<StdinLineIter>();
    }
    // Eager iterate() stays for direct consumers (list(io.stdin), sorted(io.stdin)); the for-loop
    // prefers lazyIterate above. A write stream is not iterable.
    std::optional<std::vector<Handle>> iterate(KiritoVM& vm) override {
        if (dir != Dir::In) return std::nullopt;
        RootScope rs(vm);
        std::vector<Handle> lines; std::string line;
        while (std::getline(std::cin, line)) { stripCR(line); lines.push_back(rs.add(vm.makeString(line))); }
        return lines;
    }

    std::vector<std::string> inspectMembers() const override {
        return {"write(data)", "read(size) -> String", "readline() -> String", "flush()"};
    }
    Handle getAttr(KiritoVM& vm, Handle self, std::string_view name) override {
        auto bind = [&](const char* nm, std::vector<std::string> params, NativeFn fn) {
            return makeMethod(vm, nm, std::move(params), std::move(fn), std::vector<Handle>{self});
        };
        auto me = [](KiritoVM& vm, Handle self) -> StdStream& { return static_cast<StdStream&>(vm.arena().deref(self)); };
        if (name == "write")
            return bind("write", {"data"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                Args(vm, a, "write").require(1);
                me(vm, self).streamWrite(Value(vm, a[0]).asStringRef("write"));
                return vm.none();
            });
        if (name == "read")
            return bind("read", {"size"}, [self, me](KiritoVM& vm, std::span<const Handle> a) -> Handle {
                std::optional<std::size_t> n;
                if (!a.empty()) { int64_t w = Value(vm, a[0]).asInt("read"); if (w >= 0) n = static_cast<std::size_t>(w); }
                return vm.makeString(me(vm, self).streamRead(n));
            });
        if (name == "readline")
            return bind("readline", {}, [self, me](KiritoVM& vm, std::span<const Handle>) -> Handle {
                return vm.makeString(me(vm, self).streamReadLine());
            });
        if (name == "flush")
            return bind("flush", {}, [self, me](KiritoVM& vm, std::span<const Handle>) -> Handle {
                me(vm, self).streamFlush(); return vm.none();
            });
        if (name == "_enter_")
            return bind("_enter_", {}, [self](KiritoVM&, std::span<const Handle>) { return self; });
        if (name == "_exit_" || name == "close")
            return bind("close", {}, [](KiritoVM& vm, std::span<const Handle>) { return vm.none(); });
        return Object::getAttr(vm, self, name);
    }
};

// Route output/input through whatever value is currently bound to a stream slot. The built-in stream
// types take the fast IoStream path; anything else (e.g. a user class) is duck-typed via its
// write / readline / read methods, so it can serve as stdout/stdin too.
inline void streamWriteTo(KiritoVM& vm, Handle target, const std::string& s, bool flush) {
    Object& o = vm.arena().deref(target);
    if (auto* st = dynamic_cast<IoStream*>(&o)) {
        st->streamWrite(s);
        if (flush) st->streamFlush();
        return;
    }
    RootScope rs(vm);
    Handle wf = rs.add(o.getAttr(vm, target, "write"));
    std::array<Handle, 1> args{rs.add(vm.makeString(s))};
    vm.arena().deref(wf).call(vm, args);
}
inline std::string streamReadLineFrom(KiritoVM& vm, Handle target) {
    Object& o = vm.arena().deref(target);
    if (auto* st = dynamic_cast<IoStream*>(&o)) return st->streamReadLine();
    RootScope rs(vm);
    Handle rf = rs.add(o.getAttr(vm, target, "readline"));
    return Value(vm, vm.arena().deref(rf).call(vm, {})).asStringRef("readline");
}
inline std::string streamReadFrom(KiritoVM& vm, Handle target, std::optional<std::size_t> n) {
    Object& o = vm.arena().deref(target);
    if (auto* st = dynamic_cast<IoStream*>(&o)) return st->streamRead(n);
    RootScope rs(vm);
    Handle rf = rs.add(o.getAttr(vm, target, "read"));
    if (n) { std::array<Handle, 1> a{rs.add(vm.makeInt(static_cast<int64_t>(*n)))};
             return Value(vm, vm.arena().deref(rf).call(vm, a)).asStringRef("read"); }
    return Value(vm, vm.arena().deref(rf).call(vm, {})).asStringRef("read");
}

// The `io` standard module, authored via the extension API exactly as a third party would.
class IoModule : public NativeModule {
public:
    std::string name() const override { return "io"; }

    void setup(ModuleBuilder& m) override {
        KiritoVM& vm = m.vm();
        // The three standard streams, as rebindable module members. io.print / write / input / read
        // act on whatever io.stdout / io.stdin currently hold, so `io.stdout = io.open("log","w")`
        // redirects output to a file (and rebinding back, or to io.stderr, restores it).
        // __stdout__/__stderr__/__stdin__ keep the originals so you can always rebind
        // back to the terminal: `io.stdout = io.__stdout__`.
        // Root all three across the allocations: vm.alloc collects before creating each object, so an
        // unrooted `out`/`err` held in a C++ local would be swept by the next alloc and stored dangling.
        RootScope rs(vm);
        Handle out = rs.add(vm.alloc(std::make_unique<StdStream>(StdStream::Dir::Out)));
        Handle err = rs.add(vm.alloc(std::make_unique<StdStream>(StdStream::Dir::Err)));
        Handle in = rs.add(vm.alloc(std::make_unique<StdStream>(StdStream::Dir::In)));
        m.value("stdout", out).value("__stdout__", out);
        m.value("stderr", err).value("__stderr__", err);
        m.value("stdin", in).value("__stdin__", in);
        Handle modH = m.moduleHandle();
        // Resolve a stream slot's *current* binding (looked up fresh each call, so reassignment is
        // honoured). The three slots are always present, so .at() can't miss.
        auto slot = [modH](KiritoVM& vm, const char* name) -> Handle {
            return static_cast<ModuleValue&>(vm.arena().deref(modH)).members.at(name);
        };
        // Resolve the optional `stream=` keyword (a File/BytesIO/std stream/any object with write or
        // readline); if absent, fall back to the named module slot. Other keywords are rejected.
        auto pickStream = [slot](KiritoVM& vm, std::span<const NamedArg> named, const char* dflt,
                                 const char* who) -> Handle {
            Handle chosen{};
            bool have = false;
            for (const auto& na : named) {
                if (na.name == "stream") { chosen = na.value; have = true; }
                else throw KiritoError(std::string(who) + "() got an unexpected keyword argument '" +
                                       na.name + "'");
            }
            return have ? chosen : slot(vm, dflt);
        };

        // BytesIO([initial]) -> an in-memory binary buffer.
        m.fn("BytesIO", {{"initial", "String", vm.makeString("")}}, "BytesIO",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty()) return vm.alloc(std::make_unique<BytesIO>());
            const Object& o = vm.arena().deref(a[0]);
            if (o.kind() != ValueKind::String) throw KiritoError("BytesIO expects a byte String");
            return vm.alloc(std::make_unique<BytesIO>(static_cast<const StrVal&>(o).value()));
        });
        // print(*args, stream=io.stdout): space-separated, newline-terminated, flushed. `stream=`
        // sends the output to any File/BytesIO/stream instead of the current stdout.
        m.kwfn("print", [pickStream](KiritoVM& vm, std::span<const Handle> args,
                                     std::span<const NamedArg> named) -> Handle {
            Handle target = pickStream(vm, named, "stdout", "print");
            std::string line;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) line += ' ';
                line += vm.stringify(args[i]);
            }
            line += '\n';
            // Flush so output is visible immediately on a pipe/file (a server's readiness banner,
            // progress logs) — not stuck in a fully-buffered block until exit.
            streamWriteTo(vm, target, line, /*flush=*/true);
            return vm.none();
        });
        // eprint(*args, stream=io.stderr): like print, but defaulting to the current stderr.
        m.kwfn("eprint", [pickStream](KiritoVM& vm, std::span<const Handle> args,
                                      std::span<const NamedArg> named) -> Handle {
            Handle target = pickStream(vm, named, "stderr", "eprint");
            std::string line;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) line += ' ';
                line += vm.stringify(args[i]);
            }
            line += '\n';
            streamWriteTo(vm, target, line, /*flush=*/true);
            return vm.none();
        });
        // input([prompt], stream=io.stdin): write the prompt (if any) to stdout, then read one line
        // from `stream` (a File/BytesIO/stream) or the current stdin.
        m.kwfn("input", [slot, pickStream](KiritoVM& vm, std::span<const Handle> a,
                                           std::span<const NamedArg> named) -> Handle {
            Handle src = pickStream(vm, named, "stdin", "input");
            if (!a.empty()) streamWriteTo(vm, slot(vm, "stdout"), vm.stringify(a[0]), /*flush=*/true);
            return vm.makeString(streamReadLineFrom(vm, src));
        });
        // read([n], stream=io.stdin): read n characters (or everything) from `stream` or stdin.
        m.kwfn("read", [pickStream](KiritoVM& vm, std::span<const Handle> a,
                                    std::span<const NamedArg> named) -> Handle {
            Handle src = pickStream(vm, named, "stdin", "read");
            std::optional<std::size_t> n;
            if (!a.empty()) { int64_t w = Value(vm, a[0]).asInt("read"); if (w >= 0) n = static_cast<std::size_t>(w); }
            return vm.makeString(streamReadFrom(vm, src, n));
        });
        // write(*args, stream=io.stdout): raw, no separator, no newline.
        m.kwfn("write", [pickStream](KiritoVM& vm, std::span<const Handle> args,
                                     std::span<const NamedArg> named) -> Handle {
            Handle target = pickStream(vm, named, "stdout", "write");
            std::string out;
            for (Handle h : args) out += vm.stringify(h);
            streamWriteTo(vm, target, out, /*flush=*/false);
            return vm.none();
        });
        m.fn("open", {{"path", "String"}, {"mode", "String", vm.makeString("r")}}, "File",
             [](KiritoVM& vm, std::span<const Handle> a) -> Handle {
            if (a.empty() || a.size() > 2) throw KiritoError("open expected 1 or 2 arguments");
            const Object& po = vm.arena().deref(a[0]);
            if (po.kind() != ValueKind::String) throw KiritoError("open path must be a String");
            std::string path = static_cast<const StrVal&>(po).value();
            requireNoNulPath(path, "open");  // a NUL would truncate the path (validation bypass)
            std::string mode = "r";
            if (a.size() == 2) {
                const Object& mo = vm.arena().deref(a[1]);
                if (mo.kind() != ValueKind::String) throw KiritoError("open mode must be a String");
                mode = static_cast<const StrVal&>(mo).value();
            }
            // A trailing 'b' (e.g. "rb", "wb") makes read()/readline()/iteration yield Bytes instead
            // of a String, for binary files. The underlying stream is always std::ios::binary anyway
            // (Kirito strings are byte/code-point exact, so contents and tell()/seek()/read() offsets
            // must be identical on every platform; text mode would let Windows translate \n<->\r\n).
            bool binary = false;
            if (!mode.empty() && mode.back() == 'b') { binary = true; mode.pop_back(); }
            std::ios::openmode flags = std::ios::binary;
            if (mode == "r") flags |= std::ios::in;
            else if (mode == "w") flags |= std::ios::out | std::ios::trunc;
            else if (mode == "a") flags |= std::ios::out | std::ios::app;
            else if (mode == "r+") flags |= std::ios::in | std::ios::out;
            else throw KiritoError("unsupported file mode '" + mode + (binary ? "b" : "") + "'");
            auto f = std::make_unique<FileVal>(path, flags, binary);
            if (!f->stream.is_open()) throw KiritoError("could not open file '" + path + "'");
            return vm.alloc(std::move(f));
        });

        // NB: everything that interprets, queries, mutates, or lists the filesystem by path
        // (join/dirname/basename/splitext, exists/isfile/isdir/getsize, mkdir/remove/rmtree/rename/
        // chmod, getcwd/listdir/walk) lives in the `path` module, NOT here. `io` is only I/O:
        // streams, open, print/eprint/input/read/write, and BytesIO.
    }
};

}  // namespace kirito

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#endif
