#include "lsd/script/material_graph.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsd::script {

namespace {

// A number the way both shader languages read it as a float ("1" -> "1.0").
void number(std::string& out, f32 value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.7g", static_cast<double>(value));
    out += text;
    if (!std::strpbrk(text, ".eEn")) out += ".0";  // n: nan / inf are left alone
}

// A typed value: "uv" (the mesh UVs), "x" or "x,y,z".
void literal(std::string& out, const char* value) {
    if (std::strcmp(value, "uv") == 0) {
        out += "mi.uv";
        return;
    }
    f32 v[3] = {0, 0, 0};
    const int n = std::sscanf(value, "%f , %f , %f", &v[0], &v[1], &v[2]);
    if (n == 3) {
        out += "float3(";
        number(out, v[0]);
        out += ", ";
        number(out, v[1]);
        out += ", ";
        number(out, v[2]);
        out += ")";
        return;
    }
    out += "lsd_v3(";
    number(out, n >= 1 ? v[0] : 0.0f);
    out += ")";
}

bool contains(const std::vector<u32>& list, u32 id) {
    for (u32 v : list) {
        if (v == id) return true;
    }
    return false;
}

// shortcut: std::string / std::vector here: an editor-time compiler, not a hot path.
struct Compiler {
    const Graph& graph;
    MaterialCode& out;
    char* error;
    usize error_capacity;
    std::string body;
    std::vector<u32> done;      // nodes whose outputs are declared
    std::vector<u32> visiting;  // on the current path (a loop if met again)

    bool fail(const char* message, const GraphNode* node) {
        const NodeDef* def = node ? find_node_def(node->type) : nullptr;
        if (def) std::snprintf(error, error_capacity, "%s (node '%s')", message, def->title);
        else std::snprintf(error, error_capacity, "%s", message);
        return false;
    }

    // The expression of input `pin` of `node`.
    bool input(const GraphNode& node, const NodeDef& def, u32 pin, std::string& expr) {
        if (const GraphLink* link = graph.link_into(node.id, pin)) {
            if (!declare(link->from_node)) return false;
            char name[32];
            std::snprintf(name, sizeof name, "v%u_%u", link->from_node, link->from_pin);
            expr += name;
            return true;
        }
        literal(expr, pin < kMaxPinValues ? node.values[pin] : def.inputs[pin].default_value);
        return true;
    }

    // The texture slot of a Texture Sample (shared by samples of the same texture and type).
    bool texture_slot(const GraphNode& node, u32& slot) {
        if (!node.values[1][0]) return fail("Texture Sample has no texture: type a texture asset's name", &node);
        const bool srgb = std::strcmp(node.values[2], "Normal") != 0 && std::strcmp(node.values[2], "Data") != 0;
        for (slot = 0; slot < out.texture_count; ++slot) {
            if (std::strcmp(out.textures[slot].name, node.values[1]) == 0 && out.textures[slot].srgb == srgb) return true;
        }
        if (out.texture_count == kMaxMaterialTextures) return fail("More than 3 different textures in one material", &node);
        std::snprintf(out.textures[slot].name, sizeof out.textures[slot].name, "%s", node.values[1]);
        out.textures[slot].srgb = srgb;
        ++out.texture_count;
        return true;
    }

    // The parameter slot of a Scalar / Vector Parameter (one per name).
    bool param_slot(const GraphNode& node, bool vector, u32& slot) {
        if (!node.values[0][0]) return fail("A parameter needs a Name", &node);
        for (slot = 0; slot < out.param_count; ++slot) {
            if (std::strcmp(out.params[slot].name, node.values[0]) == 0) return true;
        }
        if (out.param_count == kMaxMaterialParams) return fail("More than 16 parameters in one material", &node);
        MaterialParam& p = out.params[slot];
        std::snprintf(p.name, sizeof p.name, "%s", node.values[0]);
        p.value[0] = p.value[1] = p.value[2] = 0.0f;
        p.value[3] = 1.0f;
        const int n = std::sscanf(node.values[1], "%f , %f , %f", &p.value[0], &p.value[1], &p.value[2]);
        if (n == 1) p.value[1] = p.value[2] = p.value[0];
        p.vector = vector;
        ++out.param_count;
        return true;
    }

    // Declares a node's outputs as float3 temporaries (its inputs first).
    bool declare(u32 id) {
        if (contains(done, id)) return true;
        const GraphNode* node = graph.find(id);
        const NodeDef* def = node ? find_node_def(node->type) : nullptr;
        if (!def || !is_material_node(*def)) return fail("Not a material node", node);
        if (contains(visiting, id)) return fail("Wires go round in a loop", node);
        visiting.push_back(id);
        u32 slot = 0;
        const bool texture = std::strcmp(def->id, "MatTexture") == 0;
        if (texture && !texture_slot(*node, slot)) return false;
        const bool scalar_param = std::strcmp(def->id, "MatScalarParam") == 0, vector_param = std::strcmp(def->id, "MatVectorParam") == 0;
        if ((scalar_param || vector_param) && !param_slot(*node, vector_param, slot)) return false;
        std::string inputs[kMaxNodeInputs];
        for (u32 i = 0; i < def->input_count && !scalar_param && !vector_param; ++i) {  // parameters: the default is data, not code
            if (def->inputs[i].type == PinType::String) continue;  // settings, not values
            if (!input(*node, *def, i, inputs[i])) return false;
        }
        for (u32 o = 0; o < def->output_count; ++o) {
            char head[48];
            std::snprintf(head, sizeof head, "    float3 v%u_%u = ", id, o);
            body += head;
            const bool unpack = texture && o == 0 && std::strcmp(node->values[2], "Normal") == 0;
            if (unpack) body += "(";
            for (const char* c = def->output_code[o]; *c; ++c) {
                if (*c == '$' && c[1] >= '0' && c[1] <= '5') {
                    body += inputs[c[1] - '0'];
                    ++c;
                } else if (*c == '@') {
                    body += std::to_string(slot);
                } else {
                    body += *c;
                }
            }
            if (unpack) body += " * 2.0 - lsd_v3(1.0))";  // normal maps store -1..1 as 0..1
            body += ";\n";
        }
        visiting.pop_back();
        done.push_back(id);
        return true;
    }
};

} // namespace

bool compile_material_graph(const Graph& graph, MaterialCode& out, char* error, usize error_capacity) {
    out.code.clear();
    out.texture_count = 0;
    out.param_count = 0;
    const GraphNode* output = nullptr;
    for (const GraphNode& n : graph.nodes) {
        if (std::strcmp(n.type, "MatOutput") != 0) continue;
        if (output) {
            std::snprintf(error, error_capacity, "Two Material Output nodes: keep one");
            return false;
        }
        output = &n;
    }
    if (!output) {
        std::snprintf(error, error_capacity, "No Material Output node");
        return false;
    }
    Compiler c{graph, out, error, error_capacity, {}, {}, {}};
    const NodeDef& def = *find_node_def("MatOutput");
    // Graphs saved before a pin existed have no value for it: use the pin's default (Opacity 1, Opaque).
    GraphNode filled = *output;
    for (u32 i = 0; i < def.input_count; ++i) {
        if (!filled.values[i][0]) std::snprintf(filled.values[i], sizeof filled.values[i], "%s", def.inputs[i].default_value);
    }
    static const char* const fields[7] = {"s.base_color = ", "s.metallic = (", "s.roughness = (", "s.emissive = ", "s.normal = normalize(", "s.ao = (", "s.opacity = ("};
    static const char* const ends[7] = {";\n", ").x;\n", ").x;\n", ";\n", ");\n", ").x;\n", ").x;\n"};
    std::string assign;
    for (u32 i = 0; i < def.input_count && i < 7; ++i) {
        assign += "    ";
        assign += fields[i];
        if (!c.input(filled, def, i, assign)) return false;
        assign += ends[i];
    }
    // Blend Mode (a setting on the output, not a wire): Opaque, Masked, Translucent or Additive.
    static const char* const modes[4] = {"Opaque", "Masked", "Translucent", "Additive"};
    out.blend_mode = 0;
    for (u32 m = 0; m < 4; ++m) {
        if (std::strcmp(filled.values[7], modes[m]) == 0) out.blend_mode = m;
    }
    const std::string code = "Surface material_main(MaterialIn mi) {\n    Surface s;\n" + c.body + assign + "    return s;\n}\n";
    out.code.resize(code.size() + 1);
    std::memcpy(out.code.data(), code.c_str(), code.size() + 1);
    return true;
}

} // namespace lsd::script
