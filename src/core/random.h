// Torchlight's random number generator: a 64-bit multiply-with-carry,
//     state = 0x29777b41 * (state & 0xffffffff) + (state >> 32)
// (`g_Rand` at .bss, behind UTILITIES::randomBetween and friends).
// Seeded draws must match exactly: CQuestRewards::calculateRewards
// seeds with `quest->seed + 0x272b`.
// `setSeed(0)` takes the clock. Header-only, no dependencies.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace tl::core {

/// PORT: where `get_tick_count` reads instead of the clock when set (a test replaying another run's
/// clock); null, the clock.
inline int32_t (*&tick_count_source())() {
    static int32_t (*source)() = nullptr;
    return source;
}

/// `GetTickCount()`: milliseconds since the epoch in
/// 32-bit arithmetic, wrapping as the original's does.
inline int32_t get_tick_count() {
    if (tick_count_source() != nullptr) return tick_count_source()();
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const int64_t micros =
        std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
    const int64_t seconds = micros / 1000000;
    const int64_t usec = micros % 1000000;
    return static_cast<int32_t>(static_cast<uint32_t>(seconds) * 1000u +
                                static_cast<uint32_t>(usec / 1000));
}

/// Diagnostic sink for `--rng-trace`: one line per seeding (`S <seed>`) and per draw
/// (`F|I <lo> <hi> <state before>`), written on entry, before the early returns.
struct RandomTrace {
    void (*write)(void* user, const char* line) = nullptr;
    void* user = nullptr;
};

class RunicRandom {
public:
    explicit RunicRandom(uint32_t seed) : state_(seed) {}

    // PORT: a diagnostic trace of the draws.
    void set_trace(const RandomTrace* trace) { trace_ = trace; }

    /// `UTILITIES::setSeed(int)` and `setSeedVolatile`, inlined on
    /// the console. The seed
    /// is stored zero-extended, not sign-extended; 0 takes `GetTickCount()`.
    ///
    void set_seed(int32_t seed) {
        if (trace_ != nullptr) {
            char line[32];
            std::snprintf(line, sizeof(line), "S %d", seed);
            trace_->write(trace_->user, line);
        }
        state_ = static_cast<uint32_t>(seed != 0 ? seed : get_tick_count());
    }

    /// One multiply-with-carry step, returning the new state.
    uint64_t step() {
        state_ = kMultiplier * (state_ & 0xffffffffull) + (state_ >> 32);
        return state_;
    }

    /// `UTILITIES::randomBetween(float, float)`. Two steps
    /// make a double in [1, 2); the interpolation is done in double and narrowed at the end.
    /// `hi == lo` returns `lo` without stepping.
    float between(float lo, float hi) {
        if (trace_ != nullptr) {
            char line[96];
            std::snprintf(line, sizeof(line), "F %.9g %.9g %016llx port @%p",
                          static_cast<double>(lo), static_cast<double>(hi),
                          static_cast<unsigned long long>(state_), __builtin_return_address(0));
            trace_->write(trace_->user, line);
        }
        if (hi == lo) return lo;
        const uint64_t first = step();
        const uint64_t second = step();
        const uint64_t composed = ((first & 0xffffffffull) << 32) + (second & 0xffffffffull);
        const uint64_t bits = (composed & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
        double unit = 0.0;
        static_assert(sizeof(unit) == sizeof(bits), "double is not 64 bits");
        std::memcpy(&unit, &bits, sizeof(unit));
        return static_cast<float>(static_cast<double>(hi - lo) * (unit - 1.0)) + lo;
    }

    /// `UTILITIES::randomIntegerBetween(int, int)`.
    /// Width and sum wrap in 32 bits as on the console, so a range wider than 2^31 can
    /// exceed `hi` and be clamped to it.
    int integer_between(int lo, int hi) {
        if (trace_ != nullptr) {
            char line[96];
            std::snprintf(line, sizeof(line), "I %d %d %016llx port @%p", lo, hi,
                          static_cast<unsigned long long>(state_), __builtin_return_address(0));
            trace_->write(trace_->user, line);
        }
        if (hi <= lo) return lo;
        const uint64_t next = step();
        const uint32_t width = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo) + 1u;
        // `width` is 0 only for the whole int range, which no caller asks for.
        // NOLINTBEGIN(clang-analyzer-core.DivideZero)
        const int value =
            static_cast<int>(static_cast<uint32_t>(next) % width + static_cast<uint32_t>(lo));
        // NOLINTEND(clang-analyzer-core.DivideZero)
        return value > hi ? hi : value;
    }

    uint64_t state() const { return state_; }

    /// Puts the generator where another stands: a test can start the port's
    /// from the console's.
    void set_state(uint64_t state) { state_ = state; }

private:
    /// text. The same constant appears in all three entry points.
    static constexpr uint64_t kMultiplier = 0x29777b41ull;

    uint64_t state_ = 0;
    const RandomTrace* trace_ = nullptr;
};

/// `g_RandVolatile`: UTILITIES::randomBetweenVolatile,
/// `randomIntegerBetweenVolatile`,
/// `setSeedVolatile`: the same code over a second state, for draws allowed to
/// differ run to run (AI, combat, timers). Both generators start from the clock
/// (UTILITIES::createUniqueGuid's static init).
inline RunicRandom& volatile_random() {
    static RunicRandom generator(static_cast<uint32_t>(get_tick_count()));
    return generator;
}

/// `g_Rand`: every non-volatile draw (floor generation, random groups,
/// spawn classes, `CRandomizer` picks, quest seeds); reseeded from the clock per level.
inline RunicRandom& game_random() {
    static RunicRandom generator(static_cast<uint32_t>(get_tick_count()));
    return generator;
}

/// MSVC's `rand()`: `holdrand * 214013 + 2531011` (0x343fd, 0x269ec3),
/// per thread, 1 until `srand`. Drawn by
/// CMonster::idleAI, CCharacter::updateAnimation and
/// OGRE's `Math::UnitRandom` (so ParticleUniverse's emitters).
class MsvcRand {
public:
    explicit MsvcRand(uint32_t seed = 1) : holdrand_(seed) {}

    /// `srand(seed)`.
    void seed_with(uint32_t seed) { holdrand_ = seed; }

    /// `rand()`: 0 to 0x7fff.
    int32_t next() {
        holdrand_ = holdrand_ * 214013u + 2531011u;
        return static_cast<int32_t>((holdrand_ >> 16) & 0x7fffu);
    }

    uint32_t state() const { return holdrand_; }
    void set_state(uint32_t state) { holdrand_ = state; }

private:
    uint32_t holdrand_;
};

/// The game thread's `rand()`, at `srand(1)`'s state until `seed_crt_from_time()`.
inline MsvcRand& crt_random() {
    static MsvcRand generator(1);
    return generator;
}

/// `srand(time(0))`: `CGame::CGame` once, and
/// `CGameClient::performWarp` at every warp.
inline void seed_crt_from_time() {
    crt_random().seed_with(static_cast<uint32_t>(std::time(nullptr)));
}

/// `CGameClient::loadLevel`: both generators from the clock once the
/// room layout and spawners are chosen, before `CLevel::populate`; never for a nested layout.
inline void reseed_level_generators() {
    game_random().set_seed(0);
    volatile_random().set_seed(0);
}

}  // namespace tl::core
