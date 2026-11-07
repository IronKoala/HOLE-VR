#pragma once

namespace holevr::game::custom::transaction {
enum class Result { kApplied, kSaveRefused, kRestored, kRestoreFailed };

// Save data and the live gun must agree. After any later failure, attempt
// BOTH restorations, even when the first fails. Callers report a failed
// rollback distinctly instead of claiming the edit did not change anything.
template<class Save, class Apply, class UndoLive, class UndoSave>
Result Run(Save save, Apply apply, UndoLive undo_live, UndoSave undo_save) {
    if (!save()) return Result::kSaveRefused;
    if (apply()) return Result::kApplied;
    const bool live_ok=undo_live();
    const bool save_ok=undo_save();
    return live_ok && save_ok ? Result::kRestored : Result::kRestoreFailed;
}
}  // namespace holevr::game::custom::transaction
