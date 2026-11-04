#pragma once

namespace holevr::process_metrics {

// Call only from the existing background maintenance thread. Samples the
// whole game process (game plus mod), once immediately and then every 30 s.
// No object/heap traversal, extra worker, or per-frame work. A rising private
// commit value alone is not evidence that the mod leaked memory.
void MaybeReport();

}  // namespace holevr::process_metrics
