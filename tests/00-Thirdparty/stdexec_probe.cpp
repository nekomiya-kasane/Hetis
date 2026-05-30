// Isolated translation unit for stdexec (NVIDIA's std::execution / P2300
// reference implementation).
//
// Why its own TU: on MSVC stdexec only compiles under /Zc:preprocessor (the
// conformant preprocessor). Forcing that flag — plus stdexec's very heavy
// template instantiations — onto the monolithic main.cpp would risk the dozen
// other libraries that share that TU. Keeping stdexec here contains the blast
// radius: flip VK_TP_PROBE_STDEXEC off and this file degrades to a stub.

#if defined(VK_TP_PROBE_STDEXEC)

#include <stdexec/execution.hpp>

// Build a trivial sender pipeline (just | then) and run it synchronously.
// Exercises the core sender/receiver machinery and forces template codegen.
const char* probe_stdexec() {
    auto snd = stdexec::just(40)
             | stdexec::then([](int x) { return x + 2; });
    auto [result] = stdexec::sync_wait(std::move(snd)).value();
    return result == 42 ? "just | then -> sync_wait == 42" : "UNEXPECTED RESULT";
}

#else

const char* probe_stdexec() { return "disabled"; }

#endif
