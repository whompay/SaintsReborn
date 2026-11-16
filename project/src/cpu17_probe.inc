// 26. GUEST-THREAD SLACK PROBE (2026-10-26). Empty dist files cpu_burn_<thread>_<us> (thread = main | render | colour,
//    us = 150 | 300 | 600) add a busy spin of that many microseconds on the game main thread (once per frame, start of
//    the render kick 821888C0), on the render thread (once per frame, start of 82193A20) or on the colour worker (start
//    of the colour pass job 821915E8). The thread whose spin lowers the frame rate about 1:1 is on the critical path;
//    no change = that thread has slack. No file: one static load per call.
namespace cpu17_probe {
struct Cfg { uint32_t us[3]; bool any; };
inline const Cfg& Get() {
    static const Cfg c = [] {
        Cfg r{{0, 0, 0}, false};
        const char* names[3] = {"main", "render", "colour"};
        const uint32_t vals[3] = {150, 300, 600};
        for (int t = 0; t < 3; ++t)
            for (uint32_t v : vals) {
                char n[64];
                std::snprintf(n, sizeof(n), "cpu_burn_%s_%u", names[t], v);
                if (FileExists(n)) r.us[t] = v;
            }
        r.any = r.us[0] || r.us[1] || r.us[2];
        if (r.any)
            REXLOG_INFO("SLACK PROBE (CPU17): main thread +{} us/frame, render thread +{} us/frame, colour worker +{} us/pass",
                        r.us[0], r.us[1], r.us[2]);
        return r;
    }();
    return c;
}
inline void Burn(int t) {
    const Cfg& c = Get();
    if (!c.any || !c.us[t]) [[likely]] return;
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(c.us[t]);
    while (std::chrono::steady_clock::now() < end) _mm_pause();
}
}  // namespace cpu17_probe
