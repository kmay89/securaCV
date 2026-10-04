#pragma once
#include "canary/config.h"
#include "canary/fleet/fleet_model.h"

// Project-wide fleet singleton, sized by the flavor config (8 on watch,
// 16 on dash). Lives in fleet_instance.cpp; everything else takes it by
// reference so the model itself stays a dependency-free template.

namespace canary::fleet {

using Fleet = FleetModel<CD_FLEET_MAX_DEVICES, CD_EVENT_LOG_CAP>;

Fleet& the_fleet();

}  // namespace canary::fleet
