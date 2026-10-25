// Suspends every other thread of the process for its lifetime. Tests use it while they swap engine globals or detour
// functions that background engine threads also call (e.g. the allocator). Code run while the world is stopped must
// not allocate or take locks another thread might hold.
#pragma once

#include <cstddef>

namespace protore {

class WorldStop {
public:
    WorldStop();
    ~WorldStop();
    WorldStop(const WorldStop&) = delete;
    WorldStop& operator=(const WorldStop&) = delete;
    size_t suspended() const { return count_; }

private:
    static constexpr size_t kMaxThreads = 256;
    void* threads_[kMaxThreads];
    size_t count_ = 0;
};

}  // namespace protore
