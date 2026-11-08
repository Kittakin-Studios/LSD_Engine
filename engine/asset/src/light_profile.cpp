#include "lsd/asset/light_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lsd::asset {

bool parse_ies(const char* text, f32 out[kLightProfileSamples]) {
    const char* tilt = std::strstr(text, "TILT=");
    if (!tilt) return false;
    const bool include = std::strncmp(tilt + 5, "INCLUDE", 7) == 0;
    const char* p = std::strchr(tilt, '\n');
    if (!p) return false;
    std::vector<double> n;  // every number after the TILT line (shortcut: editor-time parsing)
    for (char* end = nullptr;; p = end) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        const double v = std::strtod(p, &end);
        if (end == p) return false;
        n.push_back(v);
    }
    usize i = 0;
    if (include) {  // lamp-to-luminaire geometry, then pairs of angles and multipliers
        if (n.size() < 2) return false;
        i = 2 + 2 * static_cast<usize>(n[1]);
    }
    if (n.size() < i + 13) return false;
    const usize vertical = static_cast<usize>(n[i + 3]), horizontal = static_cast<usize>(n[i + 4]);
    i += 13;  // 10 luminaire numbers, then ballast factor, future use, input watts
    if (vertical < 2 || horizontal < 1 || n.size() < i + vertical + horizontal + vertical * horizontal) return false;
    const double* angles = &n[i];
    const double* candela = &n[i + vertical + horizontal];
    std::vector<double> mean(vertical, 0.0);
    for (usize h = 0; h < horizontal; ++h)
        for (usize v = 0; v < vertical; ++v) mean[v] += candela[h * vertical + v] / static_cast<double>(horizontal);
    const double peak = *std::max_element(mean.begin(), mean.end());
    for (u32 s = 0; s < kLightProfileSamples; ++s) {
        const double a = 180.0 * s / (kLightProfileSamples - 1);
        double value = 0.0;
        for (usize v = 0; v + 1 < vertical; ++v) {
            if (a < angles[v] || a > angles[v + 1]) continue;
            const double span = angles[v + 1] - angles[v];
            value = span > 0.0 ? mean[v] + (mean[v + 1] - mean[v]) * (a - angles[v]) / span : mean[v];
            break;
        }
        out[s] = peak > 0.0 ? static_cast<f32>(value / peak) : 0.0f;
    }
    return true;
}

f32 light_profile_at(const f32 profile[kLightProfileSamples], f32 cos_axis) {
    const f32 x = std::acos(std::clamp(cos_axis, -1.0f, 1.0f)) / 3.14159265f * static_cast<f32>(kLightProfileSamples - 1);
    const u32 i0 = std::min(static_cast<u32>(x), kLightProfileSamples - 2);
    return profile[i0] + (profile[i0 + 1] - profile[i0]) * (x - static_cast<f32>(i0));
}

} // namespace lsd::asset
