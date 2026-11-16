// Diagnostics: the HUD timer warning beep (SYS_ACT_TIMER, sub_821FD630) in
// multiplayer. Timer struct 0x827E5EE8: +8 timer, +36 warning (beep while the
// time left is below it), +44 beep interval, +48 beeps on. Logged every 2 s
// while the timer runs, and whenever +36/+44/+48 change.
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <chrono>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/function.h>

extern "C" void __imp__sub_821FD630(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826292D0(PPCContext& ctx, uint8_t* base);

namespace {
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
}  // namespace

PPC_FUNC(sub_821FD630) {
  constexpr uint32_t t = 0x827E5EE8u;
  static uint32_t last_warn = 1, last_iv = 1, last_on = 2;
  static auto last_log = std::chrono::steady_clock::now();
  const uint32_t warn = R32(base, t + 36), iv = R32(base, t + 44), on = base[t + 48], running = R32(base, t + 8);
  const auto now = std::chrono::steady_clock::now();
  if (running && (warn != last_warn || iv != last_iv || on != last_on || now - last_log > std::chrono::seconds(2))) {
    rex::CallFrame f(ctx);
    f.ctx.r3.u64 = t + 8;
    __imp__sub_826292D0(f.ctx, base);  // time left (ms)
    REXLOG_INFO("MP TIMER: left {} ms, warn below {} ms, beep every {} ms, beeps {}, mode {}", int32_t(f.ctx.r3.u32),
                int32_t(warn), int32_t(iv), on, R32(base, 0x827D578Cu));
    last_warn = warn; last_iv = iv; last_on = on; last_log = now;
  }
  __imp__sub_821FD630(ctx, base);
}
