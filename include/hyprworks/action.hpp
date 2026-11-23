#pragma once

#include <cstdint>
#include <string>

namespace hyprworks {

enum class ActionOutcome { acknowledged, rejected, failed, cancelled };

struct ActionResult {
    std::uint64_t id = 0;
    ActionOutcome outcome = ActionOutcome::cancelled;
    bool may_have_executed = false;
    std::string error;
};

}
