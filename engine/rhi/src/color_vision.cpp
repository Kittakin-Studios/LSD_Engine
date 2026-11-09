#include "lsd/rhi/rhi.h"

namespace lsd::rhi {

namespace {

void mul3(const f32 a[9], const f32 b[9], f32 out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
}

} // namespace

// Viénot, Brettel and Mollon's dichromat simulation in LMS cone space (the published research, as
// daltonisation tools use it), then Fidaner's correction: the error a dichromat cannot see is moved
// into channels they can.
void color_vision_matrix(u32 type, f32 strength, bool correct, f32 out[9]) {
    static const f32 kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) out[i] = kIdentity[i];
    if (type < 1 || type > 3) return;
    strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
    static const f32 kRgbToLms[9] = {17.8824f, 43.5161f, 4.11935f, 3.45565f, 27.1554f, 3.86714f, 0.0299566f, 0.184309f, 1.46709f};
    static const f32 kLmsToRgb[9] = {0.0809444479f, -0.130504409f, 0.116721066f, -0.0102485335f, 0.0540193266f, -0.113614708f, -0.000365296938f, -0.00412161469f, 0.693511405f};
    static const f32 kMissing[3][9] = {
        {1, 0, 0, 0.494207f, 0, 1.24827f, 0, 0, 1},      // deuteranopia: no M cones
        {0, 2.02344f, -2.52581f, 0, 1, 0, 0, 0, 1},      // protanopia: no L cones
        {1, 0, 0, 0, 1, 0, -0.395913f, 0.801109f, 0},    // tritanopia: no S cones
    };
    f32 lms_sim[9], sim[9];
    mul3(kMissing[type - 1], kRgbToLms, lms_sim);
    mul3(kLmsToRgb, lms_sim, sim);
    for (int i = 0; i < 9; ++i) sim[i] = kIdentity[i] + strength * (sim[i] - kIdentity[i]);  // severity
    if (!correct) {
        for (int i = 0; i < 9; ++i) out[i] = sim[i];
        return;
    }
    static const f32 kShift[9] = {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1};
    f32 lost[9], shifted[9];
    for (int i = 0; i < 9; ++i) lost[i] = kIdentity[i] - sim[i];
    mul3(kShift, lost, shifted);
    for (int i = 0; i < 9; ++i) out[i] = kIdentity[i] + shifted[i];
}

void post_color_vision(const PostDraw& post, f32 constants[48]) {
    if (post.color_deficiency < 1 || post.color_deficiency > 3) return;
    f32 m[9];
    color_vision_matrix(post.color_deficiency, post.color_deficiency_strength, post.color_deficiency_correct, m);
    for (int r = 0; r < 3; ++r) {  // the composite's rows: proj, ao_p, dof (that pass has no depth effects)
        for (int c = 0; c < 3; ++c) constants[16 + r * 4 + c] = m[r * 3 + c];
    }
    constants[27] = 1.0f;
}

} // namespace lsd::rhi
