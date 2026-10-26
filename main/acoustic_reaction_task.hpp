// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <string>

#include "shared_state.hpp"

namespace stackchan::app {

void start_acoustic_reaction_task(SharedState& state, const std::string& jtts_config_json);

} // namespace stackchan::app
