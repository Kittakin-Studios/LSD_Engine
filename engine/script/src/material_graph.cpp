#include "lsd/script/material_graph.h"

#include "lsd/foundation/allocator.h"

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

MaterialFunctionResolver g_resolver = nullptr;
void* g_resolver_user = nullptr;

// shortcut: std::string / std::vector here: an editor-time compiler, not a hot path.
struct Compiler {
    const Graph& graph;
    MaterialCode& out;
    char* error;
    usize error_capacity;
    std::string body;
    std::vector<u32> done;      // nodes whose outputs are declared
    std::vector<u32> visiting;  // on the current path (a loop if met again)
    std::string prefix;                 // inside a Material Function: its temporaries' names start with this
    const std::string* args = nullptr;  // inside a Material Function: the caller's A, B, C expressions
    u32 depth = 0;

    std::string temp(u32 node, u32 pin) const { return prefix + "v" + std::to_string(node) + "_" + std::to_string(pin); }

    // A Material Function node: the named graph compiled here, its Function Inputs reading `inputs`.
    bool call_function(const GraphNode& node, const std::string* inputs) {
        if (depth >= 4) return fail("Material Functions nested more than 4 deep (does one call itself?)", &node);
        char path[300];
        if (!g_resolver || !node.values[0][0] || !g_resolver(g_resolver_user, node.values[0], path, sizeof path)) return fail("Material Function: no Material Graph with that name", &node);
        Graph sub(default_allocator());
        if (!load_graph_file(sub, path)) return fail("Material Function: could not read it", &node);
        const GraphNode* result = nullptr;
        for (const GraphNode& n : sub.nodes) {
            if (std::strcmp(n.type, "MatFunctionOutput") == 0) result = &n;
        }
        if (!result) return fail("Material Function: that graph has no Function Output node", &node);
        Compiler inner{sub, out, error, error_capacity, {}, {}, {}, prefix + "f" + std::to_string(node.id) + "_", inputs, depth + 1};
        std::string expr;
        if (!inner.input(*result, *find_node_def("MatFunctionOutput"), 0, expr)) return false;
        body += inner.body;
        body += "    float3 " + temp(node.id, 0) + " = " + expr + ";\n";
        return true;
    }

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
            expr += temp(link->from_node, link->from_pin);
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
        if (std::strcmp(def->id, "MatFunctionCall") == 0) {
            if (!call_function(*node, inputs + 1)) return false;
            visiting.pop_back();
            done.push_back(id);
            return true;
        }
        if (std::strcmp(def->id, "MatFunctionInput") == 0 && args) {  // the caller's A, B or C
            const char letter = node->values[0][0];
            const u32 which = letter == 'B' || letter == 'b' ? 1u : letter == 'C' || letter == 'c' ? 2u : 0u;
            body += "    float3 " + temp(id, 0) + " = " + args[which] + ";\n";
            visiting.pop_back();
            done.push_back(id);
            return true;
        }
        for (u32 o = 0; o < def->output_count; ++o) {
            body += "    float3 " + temp(id, o) + " = ";
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

bool parse_shader_source(const char* text, MaterialCode& out, char* error, usize error_capacity) {
    out.code.clear();
    out.texture_count = 0;
    out.param_count = 0;
    out.blend_mode = 0;
    if (!std::strstr(text, "material_main")) {
        std::snprintf(error, error_capacity, "No material_main function: a shader asset writes Surface material_main(MaterialIn mi)");
        return false;
    }
    for (const char* line = text; *line;) {
        const char* end = std::strchr(line, '\n');
        if (!end) end = line + std::strlen(line);
        const std::string l(line, end);
        line = *end ? end + 1 : end;
        const usize tex = l.find("// texture:"), par = l.find("// param:"), blend = l.find("// blend:");
        if (tex != std::string::npos) {
            char name[64] = "", kind[16] = "color";
            if (std::sscanf(l.c_str() + tex + 11, " %63s %15s", name, kind) < 1) continue;
            if (out.texture_count == kMaxMaterialTextures) {
                std::snprintf(error, error_capacity, "More than 3 textures in one shader");
                return false;
            }
            MaterialTextureSlot& t = out.textures[out.texture_count++];
            std::snprintf(t.name, sizeof t.name, "%s", name);
            t.srgb = std::strcmp(kind, "normal") != 0 && std::strcmp(kind, "data") != 0;
        } else if (par != std::string::npos) {
            char name[32] = "";
            f32 v[3] = {0, 0, 0};
            const int n = std::sscanf(l.c_str() + par + 9, " %31s %f , %f , %f", name, &v[0], &v[1], &v[2]);
            if (n < 1) continue;
            if (out.param_count == kMaxMaterialParams) {
                std::snprintf(error, error_capacity, "More than 16 parameters in one shader");
                return false;
            }
            MaterialParam& p = out.params[out.param_count++];
            std::snprintf(p.name, sizeof p.name, "%s", name);
            p.vector = n >= 4;
            p.value[0] = v[0];
            p.value[1] = p.vector ? v[1] : v[0];
            p.value[2] = p.vector ? v[2] : v[0];
            p.value[3] = 1.0f;
        } else if (blend != std::string::npos) {  // // blend: masked | translucent | additive
            const std::string mode = l.substr(blend + 9);
            out.blend_mode = mode.find("masked") != std::string::npos ? 1u : mode.find("translucent") != std::string::npos ? 2u : mode.find("additive") != std::string::npos ? 3u : 0u;
        }
    }
    const usize length = std::strlen(text);
    out.code.resize(length + 1);
    std::memcpy(out.code.data(), text, length + 1);
    return true;
}

void set_material_function_resolver(MaterialFunctionResolver resolver, void* user) {
    g_resolver = resolver;
    g_resolver_user = user;
}

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
        const GraphNode* function_result = nullptr;  // a Material Function opened alone: its result as the colour (a preview)
        for (const GraphNode& n : graph.nodes) {
            if (std::strcmp(n.type, "MatFunctionOutput") == 0) function_result = &n;
        }
        if (!function_result) {
            std::snprintf(error, error_capacity, "No Material Output node");
            return false;
        }
        Compiler preview{graph, out, error, error_capacity, {}, {}, {}, {}, nullptr, 0};
        std::string expr;
        if (!preview.input(*function_result, *find_node_def("MatFunctionOutput"), 0, expr)) return false;
        const std::string code = "Surface material_main(MaterialIn mi) {\n    Surface s;\n" + preview.body + "    s.base_color = " + expr +
                                 ";\n    s.metallic = 0.0;\n    s.roughness = 0.5;\n    s.emissive = lsd_v3(0.0);\n    s.normal = float3(0.0, 0.0, 1.0);\n"
                                 "    s.ao = 1.0;\n    s.opacity = 1.0;\n    return s;\n}\n";
        out.blend_mode = 0;
        out.code.resize(code.size() + 1);
        std::memcpy(out.code.data(), code.c_str(), code.size() + 1);
        return true;
    }
    Compiler c{graph, out, error, error_capacity, {}, {}, {}, {}, nullptr, 0};
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
