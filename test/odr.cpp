#include "backtrack.hpp"

// A second translation unit: the smoke test checks that both share one copy
// of each header-defined singleton.

std::size_t la_backtrack_odr_probe()
{
  return la::detail::base_bucket_samples;
}

la::detail::Registry* la_backtrack_odr_registry()
{
  return &la::detail::registry();
}

std::atomic<int>* la_backtrack_odr_countdown()
{
  return &la::detail::allocation_fail_countdown();
}

la::detail::GuiController* la_backtrack_odr_controller()
{
  return la::detail::GuiController::getInstanceWithoutCreating();
}
