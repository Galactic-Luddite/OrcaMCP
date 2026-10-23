#pragma once

// Cancel polling for the texture bake's stages.
//
// The caller's progress hook may only be called from the thread that called the bake (the MCP tools'
// cancel check is thread-local, the panel's job controller is not thread safe), so a stage polls from
// its own thread: serial loops every few thousand iterations (`StopPoll::tick`), parallel loops by
// handing TBB one chunk at a time and polling between chunks (`parallel_chunks`). A stage that sees the
// stop returns at once with whatever it has; the pipeline checks `stopped()` right after every stage
// and returns a cancelled result, so a half-built stage output is never used.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace TextureBake {

// Returns true when the run should stop.
using StopFn = std::function<bool()>;

// Items per chunk of a parallel loop whose body is expensive per item (a height sample, a projection):
// at -O0 about a tenth of a second on all cores, so a stop is seen within that.
inline constexpr size_t CANCEL_CHUNK = 16384;

class StopPoll
{
public:
    explicit StopPoll(const StopFn *stop = nullptr, uint32_t interval = 4096) : m_stop(stop), m_interval(interval) {}

    // For a loop body: asks the hook once per `interval` calls. Cheap enough for the innermost loop.
    bool tick()
    {
        if (m_stopped)
            return true;
        if (m_stop == nullptr || !*m_stop || ++m_count < m_interval)
            return false;
        return now();
    }
    // Asks the hook now (between stages or chunks).
    bool now()
    {
        m_count = 0;
        if (!m_stopped && m_stop != nullptr && *m_stop && (*m_stop)())
            m_stopped = true;
        return m_stopped;
    }
    bool stopped() const { return m_stopped; }
    // Whether there is a hook to ask at all; without one the loops run as before.
    bool active() const { return m_stop != nullptr && bool(*m_stop); }

private:
    const StopFn *m_stop;
    uint32_t      m_interval;
    uint32_t      m_count   = 0;
    bool          m_stopped = false;
};

// tbb::parallel_for over [0, n) in chunks of `chunk` items, polling between chunks on the calling
// thread. Same result as one parallel_for for a body that is independent per item; false when stopped
// (the items after the stopping chunk were not run). `body` takes a tbb::blocked_range<size_t>.
template<class Body>
bool parallel_chunks(size_t n, size_t chunk, StopPoll &poll, Body &&body)
{
    if (!poll.active()) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, n), body);
        return true;
    }
    for (size_t begin = 0; begin < n; begin += chunk) {
        if (poll.now())
            return false;
        tbb::parallel_for(tbb::blocked_range<size_t>(begin, std::min(n, begin + chunk)), body);
    }
    return !poll.now();
}

} // namespace TextureBake
} // namespace Slic3r
