#include "fleet_instance.h"

namespace canary::fleet {

Fleet& the_fleet() {
  static Fleet f = [] {
    Fleet fleet;
    FleetLimits l;
    l.stale_after_ms = CD_STALE_AFTER_MS;
    l.lost_after_ms  = CD_LOST_AFTER_MS;
    l.ack_hold_ms    = CD_ACK_HOLD_MS;
    fleet.set_limits(l);
    return fleet;
  }();
  return f;
}

}  // namespace canary::fleet
