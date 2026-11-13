#pragma once
// Pre-fractured meshes for Destructible objects (Unreal's Geometry Collection asset, built on demand): the object's
// Static Mesh cut into Voronoi pieces (render/fracture.h) with the component's settings, kept in
// DerivedData/Fracture/<id>.bin (keyed by the mesh, its compiled file's time, its materials and the settings),
// so a level's walls are cut once per change, not per Play. Each piece is drawn as one generated mesh per
// material section ("Fracture:<id>:<piece>:<section>" in the MeshCache); the last section is the inside.
#include "mesh_cache.h"

#include "lsd/render/fracture.h"
#include "lsd/world/components.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsd::editor {

struct FracturedMesh {
    char id[13] = "";                    // 12 hex digits of the key's hash
    std::vector<std::string> materials;  // per section; the last is the inside's
    struct Piece {
        Vec3 centre_of_mass, bounds_min, bounds_max;  // mesh space
        f32 volume = 0.0f;
        std::vector<Vec3> hull;           // convex hull points, mesh space
        std::vector<u16> sections;        // the sections it has triangles in
    };
    std::vector<Piece> pieces;
    std::vector<render::CutMesh> meshes;  // per piece (sections: 0.. materials.size() - 1)
    Vec3 bounds_min, bounds_max;
    f64 build_ms = 0.0;                   // 0 when read from DerivedData
    // The generated mesh key of a piece's section.
    void key(u32 piece, u16 section, char (&out)[40]) const;
};

class FractureCache {
public:
    explicit FractureCache(MeshCache& meshes) : meshes_(meshes) {}
    // The pieces of `mesh` cut with `d`'s settings (built now if new; their GPU meshes uploaded once), cut at the
    // object's `scale` (rounded to centimetres) so a stretched cube wall gets even pieces, not stretched ones.
    // Null if the mesh cannot be read or no piece came out.
    const FracturedMesh* get(const world::StaticMesh& mesh, const world::Destructible& d, Vec3 scale = {1, 1, 1});
    // Generates (or replaces) mesh `key` from the given pieces' triangles of one section, in mesh space: what
    // still stands of a broken object, drawn as one mesh per section. False when they have none in it.
    bool build_rest(const FracturedMesh& f, const std::vector<u32>& pieces, u16 section, const char* key);

private:
    MeshCache& meshes_;
    std::unordered_map<std::string, std::unique_ptr<FracturedMesh>> cache_;
};

// The editor's one cache (Play, the Mesh Editor's Fracture preview), made on first use.
FractureCache& fracture_cache(MeshCache& meshes);

} // namespace lsd::editor
