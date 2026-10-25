// Memory watch: find a known byte pattern in the running process, then report every instruction that reads or
// writes that range (PAGE_GUARD + vectored exception handler, re-armed after a single step).
// Diagnostic only; selected with PROTORE_PROBE=watchdir (watches the art.rcf directory once the engine loads it).
#pragma once

#include <cstddef>
#include <cstdint>

namespace protore {

// Starts a background thread that scans for `pattern` and, once found, watches `watch_len` bytes from there.
void StartMemoryWatch(const uint8_t* pattern, size_t pattern_len, size_t watch_len);

// Loads the first entries of art.rcf's directory as the pattern and starts the watch.
void StartRcfDirectoryWatch();

}  // namespace protore
