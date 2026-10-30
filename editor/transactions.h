#pragma once
// Undo / redo for the level, like Unreal's transaction system: after each finished edit the
// editor commits a labelled snapshot; undo and redo load the neighbouring snapshot back.
// shortcut: each snapshot is the whole level as text (fine for blockout-sized levels); store
// per-entity diffs when levels reach thousands of entities.
#include "lsd/foundation/array.h"
#include "lsd/world/world.h"

namespace lsd::editor {

class Transactions {
public:
    explicit Transactions(Allocator& allocator);
    ~Transactions();
    Transactions(const Transactions&) = delete;
    Transactions& operator=(const Transactions&) = delete;

    // Forgets the history; the world as it is now becomes the only state (after New / Open).
    void reset(const world::World& world, const char* label);
    // Records the world after an edit. Drops the redo branch; keeps at most kMaxHistory states.
    void commit(const world::World& world, const char* label);
    bool undo(world::World& world);
    bool redo(world::World& world);
    bool jump_to(world::World& world, u32 index);  // Undo History panel click

    bool can_undo() const { return current_ > 0; }
    bool can_redo() const { return current_ + 1 < states_.size(); }
    u32 count() const { return static_cast<u32>(states_.size()); }
    u32 current() const { return current_; }
    const char* label(u32 index) const { return states_[index].label; }

    // Unsaved changes: the current state is not the one last saved to disk.
    void mark_saved() { saved_ = current_; saved_valid_ = true; }
    bool dirty() const { return !saved_valid_ || saved_ != current_; }

    static constexpr u32 kMaxHistory = 200;

private:
    struct State {
        char* text;
        usize size;
        char label[64];
    };
    void push(const world::World& world, const char* label);
    void free_from(u32 index);

    Allocator& allocator_;
    Array<State> states_;
    u32 current_ = 0;
    u32 saved_ = 0;
    bool saved_valid_ = false;
};

} // namespace lsd::editor
