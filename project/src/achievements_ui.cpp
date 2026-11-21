// PAUSE > Achievements on PC.
//
// On the 360 the menu item (sub_822CBB50) opens the Guide's achievements
// list through sub_82717310 (XamShowAchievementsUI(user, 0)). When the call
// returns 0 the game sets its "system UI open" byte (0x8370EC09) and waits
// for the Guide to close (XN_SYS_UI false) before the menu takes input again.
// The runtime only has a stub here: it returns 0 without showing anything or
// sending XN_SYS_UI, so the menu stayed locked for the rest of the session.
//
// There are no achievements to show on PC, so the call fails: the byte stays
// 0 and the pause menu carries on as if the Guide had refused.
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <atomic>

#include <rex/logging.h>
#include <rex/ppc/function.h>

namespace {

constexpr uint32_t kErrorFunctionFailed = 0x65Bu;  // X_ERROR_FUNCTION_FAILED

std::atomic<bool> g_logged{false};

}  // namespace

// XamShowAchievementsUI(user, flags) -> an error: no Guide list on PC.
PPC_FUNC_IMPL(__imp__XamShowAchievementsUI) {
  if (!g_logged.exchange(true))
    REXLOG_INFO("Achievements UI: no achievements list on PC (user {}); the menu carries on", ctx.r3.s32);
  ctx.r3.u64 = kErrorFunctionFailed;
}
