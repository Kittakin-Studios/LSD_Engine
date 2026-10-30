// Editor tests that run without a GPU: undo / redo history and unsaved-change tracking.
#include "transactions.h"

#include "lsd/world/components.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

using namespace lsd;

static void test_undo_redo() {
    HeapAllocator heap("editor");
    {
        world::World w(heap);
        editor::Transactions history(heap);
        const world::Entity box = w.create("Box");
        history.reset(w, "Open Level");
        CHECK(!history.dirty() && !history.can_undo());

        w.get<world::Transform>(box)->position = {3, 0, 0};
        history.commit(w, "Move");
        const world::Entity ball = w.create("Ball");
        history.commit(w, "Place Ball");
        CHECK(history.dirty() && history.count() == 3 && std::strcmp(history.label(2), "Place Ball") == 0);

        CHECK(history.undo(w));
        CHECK(!w.alive(ball) && w.alive(box) && w.get<world::Transform>(box)->position.x == 3.0f);
        CHECK(history.undo(w));
        CHECK(w.get<world::Transform>(box)->position.x == 0.0f && !history.dirty());  // back at the saved state
        CHECK(!history.undo(w));
        CHECK(history.redo(w) && history.redo(w) && w.alive(ball));  // same handle comes back
        CHECK(!history.redo(w));

        history.mark_saved();
        history.undo(w);
        w.destroy(box);
        history.commit(w, "Delete Box");  // drops the redo branch that held the saved state
        CHECK(!history.can_redo() && history.dirty() && !w.alive(box));
        CHECK(history.jump_to(w, 0) && w.alive(box) && !w.alive(ball));
    }
    CHECK(heap.allocated_bytes() == 0);
}

int main() {
    test_undo_redo();
    if (g_failures == 0) std::printf("All editor tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
