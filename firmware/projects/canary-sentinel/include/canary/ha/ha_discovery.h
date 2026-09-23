#pragma once
#include <PubSubClient.h>
#include "canary/topics.h"

namespace canary::ha {
  void publish_discovery(PubSubClient& mqtt, const Topics& topics);
}
