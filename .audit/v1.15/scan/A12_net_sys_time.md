# A12 Net/Sys/Time

Status: IN PROGRESS

Scope: src/kirito/stdlib_net.hpp, src/kirito/net_compat.hpp, src/kirito/stdlib_sys.hpp,
src/kirito/proc_compat.hpp, src/kirito/stdlib_time.hpp.

Reviewed README.md false-positives table first (sys.exit skips fstreams — noted, will not re-flag).

Read in full: stdlib_net.hpp (1600 lines), net_compat.hpp, stdlib_sys.hpp, proc_compat.hpp,
stdlib_time.hpp. Cross-checked against test_net.cpp, test_net_primitives.cpp, test_net_tls.cpp,
test_proc.cpp, test_sys.cpp, test_sys_binary.cpp, test_sys_time_deep.cpp, test_time.cpp, and the
`.ki` golden scripts (verify_net/labx_net/r6-r10_net/spec_net*/probe_http_client_abuse/audit_net,
sys_proc*, verify_time/labx_sys_time/r6_time/audit_time/deep_system).

Overall impression: this subsystem has been through several prior audit rounds (v1.14, v1.14.1,
1.12.1) and is unusually hardened already — overflow-checked DateTime arithmetic, a portable
civil-calendar epoch<->fields conversion independent of platform gmtime range limits, a
grandchild-killing process-group timeout for createprocess/shell, an exec-error pipe for clean
"program not found" errors, drain threads with capture caps to avoid OOM/deadlock, CRLF-injection
guards on HTTP headers/cookies/multipart fields, and an explicit credential-stripping redirect
policy (NET-1). Findings below are the residual gaps found after this baseline.



