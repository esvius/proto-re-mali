// binkw32.dll for the native host. No decoder yet: BinkOpen fails, so the engine skips its movies (intro logos,
// cutscenes). The real implementation will decode with FFmpeg's bink/binkaudio decoders.
#include <chrono>

#include "guest.h"

namespace native {

namespace {

auto g_start = std::chrono::steady_clock::now();

#define B(name) void b_##name(Ctx* c)

B(BinkSetMemory) { Return(c, 0, 2); }
B(BinkSetIO) { Return(c, 0, 1); }
B(BinkSetSoundSystem) { Return(c, 1, 2); }
B(BinkSetSoundTrack) { Return(c, 0, 2); }
B(BinkOpen) {
    Log("bink: BinkOpen(%s) -> no decoder, movie skipped", Arg(c, 0) ? Str(Arg(c, 0)) : "");
    Return(c, 0, 2);
}
B(BinkClose) { Return(c, 0, 1); }
B(BinkWait) { Return(c, 0, 1); }
B(BinkDoFrame) { Return(c, 0, 1); }
B(BinkNextFrame) { Return(c, 0, 1); }
B(BinkPause) { Return(c, 0, 2); }
B(BinkGetFrameBuffersInfo) { Return(c, 0, 2); }
B(BinkRegisterFrameBuffers) { Return(c, 0, 2); }
B(BinkSetVolume) { Return(c, 0, 3); }
B(BinkSetMixBins) { Return(c, 0, 4); }
B(BinkOpenTrack) { Return(c, 0, 2); }
B(BinkCloseTrack) { Return(c, 0, 1); }
B(RADTimerRead) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start);
    Return(c, uint32_t(ms.count()), 0);
}

#undef B

#define E(name, args) {"binkw32.dll", "_" #name "@" #args, b_##name}
const ImportDef kBink[] = {
    E(BinkSetMemory, 8), E(BinkSetIO, 4), E(BinkSetSoundSystem, 8), E(BinkSetSoundTrack, 8), E(BinkOpen, 8),
    E(BinkClose, 4), E(BinkWait, 4), E(BinkDoFrame, 4), E(BinkNextFrame, 4), E(BinkPause, 8),
    E(BinkGetFrameBuffersInfo, 8), E(BinkRegisterFrameBuffers, 8), E(BinkSetVolume, 12), E(BinkSetMixBins, 16),
    E(BinkOpenTrack, 8), E(BinkCloseTrack, 4), E(RADTimerRead, 0),
};
#undef E
ImportRegistrar g_bink(kBink);

}  // namespace

}  // namespace native
