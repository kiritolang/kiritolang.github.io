// embed_physics_step.cpp — a 1-D particle integrator with pluggable forces. C++ owns the particle
// state (pos, vel as doubles) and the semi-implicit Euler timestep loop; Kirito owns the FORCE law,
// a Function(state: Dict) -> Float that returns the acceleration for {"pos", "vel", "t"}. Swap the
// Kirito function and the same C++ engine integrates a different physical system (constant gravity,
// a damped spring, …).
//
// Flow per step: C++ (build state Dict) → Kirito (force → acceleration) → C++ (semi-implicit Euler
// update of vel then pos).

#include <array>
#include <string>
#include <vector>

#include "../check.hpp"
#include "kirito.hpp"

using namespace kirito;

struct Particle {
    double pos;
    double vel;
};

// The integrator: pure C++ control loop, Kirito-owned force. Semi-implicit (symplectic) Euler:
//   a = force(state);  vel += a*dt;  pos += vel*dt.
class Integrator {
public:
    // Pin the compiled force-function handle: it came from a throwaway runSource module scope, so
    // nothing else roots it, and `accel` allocates enough per step to trigger a GC that would
    // otherwise sweep it (dangling handle) mid-run.
    Integrator(KiritoVM& vm, Handle force) : vm_(vm), force_(force) { vm_.pinHandle(force_); }
    ~Integrator() { vm_.unpinHandle(force_); }
    Integrator(const Integrator&) = delete;
    Integrator& operator=(const Integrator&) = delete;

    // Advance the particle `steps` times with timestep `dt`, returns the final state. Records the
    // trajectory into `trace` if non-null (for post-hoc trajectory checks).
    Particle run(Particle p, double dt, int steps, std::vector<Particle>* trace = nullptr) {
        double t = 0.0;
        for (int i = 0; i < steps; ++i) {
            double a = accel(p, t);
            p.vel += a * dt;
            p.pos += p.vel * dt;
            t += dt;
            if (trace) trace->push_back(p);
        }
        return p;
    }

    // One force evaluation: hand the current state to Kirito, demand a numeric acceleration back.
    double accel(const Particle& p, double t) {
        RootScope rs(vm_);
        Dict state(vm_);
        state.set("pos", Value(vm_, p.pos));
        state.set("vel", Value(vm_, p.vel));
        state.set("t",   Value(vm_, t));
        std::array<Handle, 1> args{rs.add(state.handle())};
        Value r(vm_, rs.add(vm_.arena().deref(force_).call(vm_, args)));
        // The force law MUST return a number — a broken law (wrong type) fails loudly, not silently.
        if (!r.isNumber())
            throw KiritoError("force law must return a Float, got '" + r.typeName() + "'");
        return r.asFloat("acceleration");
    }

private:
    KiritoVM& vm_;
    Handle    force_;
};

int main() {
    KiritoVM vm;
    auto compile = [&](const char* src) { return vm.runSource(src); };

    // ---- Force 1: constant gravity. a = -g, independent of state. ----
    Handle gravity = compile(R"KI(
Function(state) -> Float:
    return -9.8
)KI");

    // ---- Force 2: a damped spring. a = -k*pos - c*vel (Hooke's law + linear drag). ----
    Handle spring = compile(R"KI(
Function(state) -> Float:
    var k = 4.0
    var c = 0.5
    return -k * state["pos"] - c * state["vel"]
)KI");

    const double dt = 0.001;

    // ==== Scenario A: gravity — a body dropped from rest falls quadratically. ====
    // Analytic solution of semi-implicit Euler under constant a=-g after N steps:
    //   vel = -g*N*dt,  pos ~ -0.5*g*t^2 (to O(dt)). We check the physics holds to a loose tol.
    {
        Integrator engine(vm, gravity);
        Particle start{0.0, 0.0};
        int steps = 1000;                 // t = 1.0 s
        Particle end = engine.run(start, dt, steps);
        double t = steps * dt;

        // Velocity is exactly -g*t for constant acceleration under this scheme.
        CHECK(Float(vm, end.vel).compare(Value(vm, -9.8 * t), 1e-9, 1e-6));

        // Position fell by ~ 0.5*g*t^2 = 4.9 m; semi-implicit Euler overshoots by O(dt), so a
        // slightly loose absolute tolerance. It must be NEGATIVE (fell downward) and near -4.9.
        CHECK(end.pos < 0.0);
        CHECK(Float(vm, end.pos).compare(Value(vm, -4.9), 1e-3, 1e-2));

        // Monotonic descent: every recorded position is below the previous one.
        std::vector<Particle> trace;
        engine.run(start, dt, 200, &trace);
        for (std::size_t i = 1; i < trace.size(); ++i)
            CHECK(trace[i].pos < trace[i - 1].pos);
    }

    // ==== Scenario B: damped spring — energy decays, the particle settles at the origin. ====
    // Released from pos=1, vel=0 with drag c>0, the oscillation loses energy and pos → 0.
    {
        Integrator engine(vm, spring);
        Particle start{1.0, 0.0};

        // Total mechanical energy E = 0.5*vel^2 + 0.5*k*pos^2 (unit mass, k=4). It must decrease.
        auto energy = [](const Particle& p) {
            const double k = 4.0;
            return 0.5 * p.vel * p.vel + 0.5 * k * p.pos * p.pos;
        };
        double e0 = energy(start);

        std::vector<Particle> trace;
        Particle end = engine.run(start, dt, 20000, &trace);   // t = 20 s, many periods
        double eEnd = energy(end);

        // Energy strictly decayed away.
        CHECK(eEnd < e0);
        // And decayed to nearly nothing — the system is heavily settled after 20 s.
        CHECK(eEnd < 0.01);

        // The particle converged to the origin (equilibrium of the undriven spring). Energy < 0.01
        // bounds |vel| < sqrt(0.02) ≈ 0.14 and |pos| < sqrt(0.005) ≈ 0.07, so use tolerances that
        // reflect the energy budget rather than a tighter number the settle doesn't guarantee.
        CHECK(Float(vm, end.pos).compare(Value(vm, 0.0), 1e-9, 0.1));
        CHECK(Float(vm, end.vel).compare(Value(vm, 0.0), 1e-9, 0.2));

        // Energy is (weakly) monotone non-increasing on a coarse sub-sample — sample every 1000
        // steps so per-step round-off in this near-conservative scheme doesn't trip the check.
        double prev = e0;
        for (std::size_t i = 999; i < trace.size(); i += 1000) {
            double e = energy(trace[i]);
            CHECK(e <= prev + 1e-9);
            prev = e;
        }

        // It genuinely oscillated on the way down (crossed the origin at least once), not just crept.
        bool crossed = false;
        for (std::size_t i = 1; i < trace.size(); ++i)
            if ((trace[i].pos < 0.0) != (trace[i - 1].pos < 0.0)) { crossed = true; break; }
        CHECK(crossed);
    }

    // ==== Scenario C: state visibility — a time-dependent force sees state["t"] advance. ====
    // a = t makes velocity grow as ~0.5*t^2; confirms the engine really feeds the clock through.
    {
        Handle ramp = compile(R"KI(
Function(state) -> Float:
    return state["t"]
)KI");
        Integrator engine(vm, ramp);
        Particle end = engine.run({0.0, 0.0}, dt, 1000, nullptr);   // t → 1.0
        // vel = sum over steps of t_i*dt ≈ integral_0^1 t dt = 0.5.
        CHECK(Float(vm, end.vel).compare(Value(vm, 0.5), 1e-3, 1e-3));
        CHECK(end.pos > 0.0);
    }

    // ---- adversarial: a force law that returns a non-number is rejected loudly. ----
    {
        Integrator bad(vm, compile("Function(state): return \"a lot of force\"\n"));
        CHECK_THROWS(bad.accel({0.0, 0.0}, 0.0));
    }

    // ---- adversarial: a force law that trips a math domain error (sqrt of a negative) throws,
    //      and the exception crosses the C++ boundary. ----
    {
        Handle boom = compile(R"KI(
Function(state):
    var math = import("math")
    return math.sqrt(-1.0)
)KI");
        Integrator explode(vm, boom);
        CHECK_THROWS(explode.accel({0.0, 0.0}, 0.0));
    }

    // ---- adversarial: a force law that indexes a missing key throws. ----
    {
        Integrator missing(vm, compile("Function(state) -> Float: return state[\"mass\"]\n"));
        CHECK_THROWS(missing.accel({0.0, 0.0}, 0.0));
    }

    return RUN_TESTS();
}
