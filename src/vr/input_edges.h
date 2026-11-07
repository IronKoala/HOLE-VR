#pragma once
// Button edges queued between headset samples and game-thread consumption.
// Current button levels and poses live in the snapshot, not in this queue.

namespace holevr::vr::input {

struct ButtonEdges {
    bool pressed = false;
    bool released = false;
};

inline bool GameplayButtonDown(bool raw_down, const ButtonEdges& edges) {
    // A release followed by a UI-owned press may end with the physical button
    // down. Preserve the release for gameplay for this consumption, rather than
    // letting a pre-menu hold continue on the UI's new squeeze. A normal batch
    // that still contains a gameplay press keeps its actual final level.
    return raw_down && !(edges.released && !edges.pressed);
}

class PendingButtonEdges {
public:
    void Observe(int hand, int button, bool was_down, bool down) {
        auto& edge = pending_[hand][button];
        edge.pressed = edge.pressed || (down && !was_down);
        edge.released = edge.released || (!down && was_down);
    }
    ButtonEdges Take(int hand, int button) {
        const auto edge = pending_[hand][button];
        pending_[hand][button] = {};
        return edge;
    }
    void DiscardPresses() {
        // UI-owned presses must not replay after a click closes a menu.
        // Releases are retained so preexisting gameplay holds can clean up.
        for (auto& hand : pending_)
            for (auto& edge : hand) edge.pressed = false;
    }

private:
    ButtonEdges pending_[2][6]{};
};

}  // namespace holevr::vr::input
