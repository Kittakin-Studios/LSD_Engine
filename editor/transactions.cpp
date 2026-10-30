#include "transactions.h"

#include "lsd/world/level.h"

#include <cstdio>
#include <cstring>

namespace lsd::editor {

Transactions::Transactions(Allocator& allocator) : allocator_(allocator), states_(allocator) {}

Transactions::~Transactions() { free_from(0); }

void Transactions::free_from(u32 index) {
    for (u32 i = index; i < states_.size(); ++i) allocator_.deallocate(states_[i].text);
    if (index < states_.size()) states_.resize(index);
}

void Transactions::push(const world::World& world, const char* label) {
    Array<char> text(allocator_);
    world::save_level(world, text);
    State state;
    state.size = text.size();
    state.text = static_cast<char*>(allocator_.allocate(state.size + 1, 1));
    std::memcpy(state.text, text.data(), state.size);
    std::snprintf(state.label, sizeof state.label, "%s", label);
    states_.push_back(state);
}

void Transactions::reset(const world::World& world, const char* label) {
    free_from(0);
    push(world, label);
    current_ = 0;
    mark_saved();
}

void Transactions::commit(const world::World& world, const char* label) {
    if (saved_valid_ && saved_ > current_) saved_valid_ = false;  // the saved state is in the dropped redo branch
    free_from(current_ + 1);
    push(world, label);
    if (states_.size() > kMaxHistory) {
        allocator_.deallocate(states_[0].text);
        for (usize i = 0; i + 1 < states_.size(); ++i) states_[i] = states_[i + 1];
        states_.resize(states_.size() - 1);
        if (saved_ == 0) saved_valid_ = false;
        else --saved_;
    }
    current_ = static_cast<u32>(states_.size() - 1);
}

bool Transactions::jump_to(world::World& world, u32 index) {
    if (index >= states_.size() || index == current_) return false;
    current_ = index;
    return world::load_level(world, states_[index].text, states_[index].size);
}

bool Transactions::undo(world::World& world) { return can_undo() && jump_to(world, current_ - 1); }

bool Transactions::redo(world::World& world) { return can_redo() && jump_to(world, current_ + 1); }

} // namespace lsd::editor
