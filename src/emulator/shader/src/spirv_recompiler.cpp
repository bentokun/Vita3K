// Vita3K emulator project
// Copyright (C) 2018 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <shader/spirv_recompiler.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <shader/profile.h>
#include <shader/usse_translator_entry.h>
#include <util/fs.h>
#include <util/log.h>

#include <SPIRV/SpvBuilder.h>
#include <SPIRV/disassemble.h>
#include <boost/optional.hpp>
#include <spirv_glsl.hpp>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

using boost::optional;
using SpirvCode = std::vector<uint32_t>;

static constexpr bool LOG_SHADER_DEBUG = true;

namespace shader {

// **************
// * Prototypes *
// **************

spv::Id get_type_fallback(spv::Builder &b);

// ******************
// * Helper structs *
// ******************

// Keeps track of current struct declaration
// TODO: Handle struct arrays and multiple struct instances.
//       The current (and the former) approach is quite naive, in that it assumes:
//           1) there is only one struct instance per declared struct
//           2) there are no struct array instances
struct StructDeclContext {
    std::string name;
    USSE::RegisterBank reg_type = USSE::RegisterBank::INVALID;
    std::vector<spv::Id> field_ids;
    std::vector<std::string> field_names; // count must be equal to `field_ids`
    bool is_interaface_block{ false };

    bool empty() const { return name.empty(); }
    void clear() { *this = {}; }
};

struct VertexProgramOutputProperties {
    std::string name;
    std::uint32_t component_count;

    VertexProgramOutputProperties()
        : name(nullptr)
        , component_count(0) {}

    VertexProgramOutputProperties(const char *name, std::uint32_t component_count)
        : name(name)
        , component_count(component_count) {}
};
using VertexProgramOutputPropertiesMap = std::map<SceGxmVertexProgramOutputs, VertexProgramOutputProperties>;

struct FragmentProgramInputProperties {
    std::string name;
    std::uint32_t component_count;

    FragmentProgramInputProperties()
        : name(nullptr)
        , component_count(0) {}

    FragmentProgramInputProperties(const char *name, std::uint32_t component_count)
        : name(name)
        , component_count(component_count) {}
};
using FragmentProgramInputPropertiesMap = std::map<SceGxmFragmentProgramInputs, FragmentProgramInputProperties>;

// ******************************
// * Functions (implementation) *
// ******************************

static spv::Id create_array_if_needed(spv::Builder &b, const spv::Id param_id, const SceGxmProgramParameter &parameter, const uint32_t explicit_array_size = 0) {
    // disabled
    return param_id;

    const auto array_size = explicit_array_size == 0 ? parameter.array_size : explicit_array_size;
    if (array_size > 1) {
        const auto array_size_id = b.makeUintConstant(array_size);
        return b.makeArrayType(param_id, array_size_id, 0);
    }
    return param_id;
}

static spv::Id get_type_basic(spv::Builder &b, const SceGxmProgramParameter &parameter) {
    SceGxmParameterType type = gxp::parameter_type(parameter);

    switch (type) {
    // clang-format off
    case SCE_GXM_PARAMETER_TYPE_F16:return b.makeFloatType(32); // TODO: support f16
    case SCE_GXM_PARAMETER_TYPE_F32: return b.makeFloatType(32);
    case SCE_GXM_PARAMETER_TYPE_U8: return b.makeUintType(8);
    case SCE_GXM_PARAMETER_TYPE_U16: return b.makeUintType(16);
    case SCE_GXM_PARAMETER_TYPE_U32: return b.makeUintType(32);
    case SCE_GXM_PARAMETER_TYPE_S8: return b.makeIntType(8);
    case SCE_GXM_PARAMETER_TYPE_S16: return b.makeIntType(16);
    case SCE_GXM_PARAMETER_TYPE_S32: return b.makeIntType(32);
    // clang-format on
    default: {
        LOG_ERROR("Unsupported parameter type {} used in shader.", log_hex(type));
        return get_type_fallback(b);
    }
    }
}

static spv::Id get_type_fallback(spv::Builder &b) {
    return b.makeFloatType(32);
}

static spv::Id get_type_scalar(spv::Builder &b, const SceGxmProgramParameter &parameter) {
    spv::Id param_id = get_type_basic(b, parameter);
    param_id = create_array_if_needed(b, param_id, parameter);
    return param_id;
}

static spv::Id get_type_vector(spv::Builder &b, const SceGxmProgramParameter &parameter) {
    spv::Id param_id = get_type_basic(b, parameter);
    param_id = b.makeVectorType(param_id, parameter.component_count);
    return param_id;
}

// disabled
static spv::Id get_type_matrix(spv::Builder &b, const SceGxmProgramParameter &parameter) {
    spv::Id param_id = get_type_basic(b, parameter);

    // There's no information on whether the parameter was a matrix originally (such type info is lost)
    // so attempt to make a NxN matrix, or a NxN matrix array of size M if possible (else fall back to vector array)
    // where N = parameter.component_count
    //       M = matrix_array_size
    const auto total_type_size = parameter.component_count * parameter.array_size;
    const auto matrix_size = parameter.component_count * parameter.component_count;
    const auto matrix_array_size = total_type_size / matrix_size;
    const auto matrix_array_size_leftover = total_type_size % matrix_size;

    if (matrix_array_size_leftover == 0) {
        param_id = b.makeMatrixType(param_id, parameter.component_count, parameter.component_count);
        param_id = create_array_if_needed(b, param_id, parameter, matrix_array_size);
    } else {
        // fallback to vector array
        param_id = get_type_vector(b, parameter);
    }

    return param_id;
}

static spv::Id get_param_type(spv::Builder &b, const SceGxmProgramParameter &parameter) {
    std::string name = gxp::parameter_name_raw(parameter);
    gxp::GenericParameterType param_type = gxp::parameter_generic_type(parameter);

    switch (param_type) {
    case gxp::GenericParameterType::Scalar:
        return get_type_scalar(b, parameter);
    case gxp::GenericParameterType::Vector:
        return get_type_vector(b, parameter);
    case gxp::GenericParameterType::Matrix: // disabled
        return get_type_matrix(b, parameter);
    default:
        return get_type_fallback(b);
    }
}

static void sanitize_variable_name(std::string &var_name) {
    // Remove consecutive occurences of the character '_'
    var_name.erase(
        std::unique(var_name.begin(), var_name.end(),
            [](char c1, char c2) { return c1 == c2 && c2 == '_'; }),
        var_name.end());
}

spv::StorageClass reg_type_to_spv_storage_class(USSE::RegisterBank reg_type) {
    switch (reg_type) {
    case USSE::RegisterBank::TEMP:
        return spv::StorageClassFunction;
    case USSE::RegisterBank::PRIMATTR:
        return spv::StorageClassInput;
    case USSE::RegisterBank::OUTPUT:
        return spv::StorageClassOutput;
    case USSE::RegisterBank::SECATTR:
        return spv::StorageClassUniformConstant;
    case USSE::RegisterBank::FPINTERNAL:
        return spv::StorageClassPrivate;

    case USSE::RegisterBank::SPECIAL: break;
    case USSE::RegisterBank::GLOBAL: break;
    case USSE::RegisterBank::FPCONSTANT: break;
    case USSE::RegisterBank::IMMEDIATE: break;
    case USSE::RegisterBank::INDEX: break;
    case USSE::RegisterBank::INDEXED: break;

    case USSE::RegisterBank::MAXIMUM:
    case USSE::RegisterBank::INVALID:
    default:
        return spv::StorageClassMax;
    }

    LOG_WARN("Unsupported reg_type {}", static_cast<uint32_t>(reg_type));
    return spv::StorageClassMax;
}

static spv::Id create_variable(spv::Builder &b, SpirvShaderParameters &parameters, std::string &name, USSE::RegisterBank reg_type, uint32_t size, spv::Id type) {
    sanitize_variable_name(name);

    const auto storage_class = reg_type_to_spv_storage_class(reg_type);
    spv::Id var_id = b.createVariable(storage_class, type, name.c_str());

    SpirvVarRegBank *var_group;

    switch (reg_type) {
    case USSE::RegisterBank::SECATTR:
        var_group = &parameters.uniforms;
        break;
    case USSE::RegisterBank::PRIMATTR:
        var_group = &parameters.ins;
        break;
    case USSE::RegisterBank::OUTPUT:
        var_group = &parameters.outs;
        break;
    case USSE::RegisterBank::TEMP:
        var_group = &parameters.temps;
        break;
    case USSE::RegisterBank::FPINTERNAL:
        var_group = &parameters.internals;
        break;
    default:
        LOG_WARN("Unsupported reg_type {}", static_cast<uint32_t>(reg_type));
        return spv::NoResult;
    }

    var_group->push({ type, var_id }, size);

    return var_id;
}

static spv::Id create_struct(spv::Builder &b, SpirvShaderParameters &parameters, StructDeclContext &param_struct, emu::SceGxmProgramType program_type) {
    assert(param_struct.field_ids.size() == param_struct.field_names.size());

    const spv::Id struct_type_id = b.makeStructType(param_struct.field_ids, param_struct.name.c_str());

    // NOTE: This will always be true until we support uniform structs (see comment below)
    if (param_struct.is_interaface_block)
        b.addDecoration(struct_type_id, spv::DecorationBlock);

    for (auto field_index = 0; field_index < param_struct.field_ids.size(); ++field_index) {
        const auto field_name = param_struct.field_names[field_index];

        b.addMemberName(struct_type_id, field_index, field_name.c_str());
    }

    // TODO: Size doesn't make sense here, so just use 1
    const spv::Id struct_var_id = create_variable(b, parameters, param_struct.name, param_struct.reg_type, 1, struct_type_id);

    param_struct.clear();
    return struct_var_id;
}

static spv::Id create_param_sampler(spv::Builder &b, SpirvShaderParameters &parameters, const SceGxmProgramParameter &parameter) {
    spv::Id sampled_type = b.makeFloatType(32);
    spv::Id image_type = b.makeImageType(sampled_type, spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
    spv::Id sampled_image_type = b.makeSampledImageType(image_type);
    std::string name = gxp::parameter_name_raw(parameter);

    return create_variable(b, parameters, name, USSE::RegisterBank::SECATTR, 2, sampled_image_type);
}

static void create_vertex_outputs(spv::Builder &b, SpirvShaderParameters &parameters, const SceGxmProgram &program) {
    auto set_property = [](SceGxmVertexProgramOutputs vo, const char *name, std::uint32_t component_count) {
        return std::make_pair(vo, VertexProgramOutputProperties(name, component_count));
    };

    // TODO: Verify component counts
    static const VertexProgramOutputPropertiesMap vertex_properties_map = {
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_POSITION, "out_Position", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_FOG, "out_Fog", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_COLOR0, "out_Color0", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_COLOR1, "out_Color1", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD0, "out_TexCoord0", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD1, "out_TexCoord1", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD2, "out_TexCoord2", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD3, "out_TexCoord3", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD4, "out_TexCoord4", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD5, "out_TexCoord5", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD6, "out_TexCoord6", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD7, "out_TexCoord7", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD8, "out_TexCoord8", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_TEXCOORD9, "out_TexCoord9", 2),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_PSIZE, "out_Psize", 1),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP0, "out_Clip0", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP1, "out_Clip1", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP2, "out_Clip2", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP3, "out_Clip3", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP4, "out_Clip4", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP5, "out_Clip5", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP6, "out_Clip6", 4),
        set_property(SCE_GXM_VERTEX_PROGRAM_OUTPUT_CLIP7, "out_Clip7", 4),
    };

    const SceGxmVertexProgramOutputs vertex_outputs = gxp::get_vertex_outputs(program);

    for (int vo = SCE_GXM_VERTEX_PROGRAM_OUTPUT_POSITION; vo < _SCE_GXM_VERTEX_PROGRAM_OUTPUT_LAST; vo <<= 1) {
        if (vertex_outputs & vo) {
            const auto vo_typed = static_cast<SceGxmVertexProgramOutputs>(vo);
            VertexProgramOutputProperties properties = vertex_properties_map.at(vo_typed);

            const spv::Id out_type = b.makeVectorType(b.makeFloatType(32), properties.component_count);
            const spv::Id out_var = create_variable(b, parameters, properties.name, USSE::RegisterBank::OUTPUT, properties.component_count, out_type);

            // TODO: More decorations needed?
            if (vo == SCE_GXM_VERTEX_PROGRAM_OUTPUT_POSITION)
                b.addDecoration(out_var, spv::DecorationBuiltIn, spv::BuiltInPosition);
        }
    }
}

static void create_fragment_inputs(spv::Builder &b, SpirvShaderParameters &parameters, const SceGxmProgram &program) {
    auto set_property = [](SceGxmFragmentProgramInputs vo, const char *name, std::uint32_t component_count) {
        return std::make_pair(vo, FragmentProgramInputProperties(name, component_count));
    };

    // TODO: Verify component counts
    static const FragmentProgramInputPropertiesMap vertex_properties_map = {
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_POSITION, "in_Position", 4),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_FOG, "in_Fog", 4),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_COLOR0, "in_Color0", 4),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_COLOR1, "in_Color1", 4),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD0, "in_TexCoord0", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD1, "in_TexCoord1", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD2, "in_TexCoord2", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD3, "in_TexCoord3", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD4, "in_TexCoord4", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD5, "in_TexCoord5", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD6, "in_TexCoord6", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD7, "in_TexCoord7", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD8, "in_TexCoord8", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_TEXCOORD9, "in_TexCoord9", 2),
        set_property(SCE_GXM_FRAGMENT_PROGRAM_INPUT_SPRITECOORD, "in_SpriteCoord", 2),
    };

    const SceGxmFragmentProgramInputs fragment_inputs = gxp::get_fragment_inputs(program);

    for (int vo = SCE_GXM_FRAGMENT_PROGRAM_INPUT_POSITION; vo < _SCE_GXM_FRAGMENT_PROGRAM_INPUT_LAST; vo <<= 1) {
        if (fragment_inputs & vo) {
            const auto vo_typed = static_cast<SceGxmFragmentProgramInputs>(vo);
            FragmentProgramInputProperties properties = vertex_properties_map.at(vo_typed);

            const spv::Id in_type = b.makeVectorType(b.makeFloatType(32), properties.component_count);
            const spv::Id in_var = create_variable(b, parameters, properties.name, USSE::RegisterBank::PRIMATTR, properties.component_count, in_type);
        }
    }
}

static void create_fragment_output(spv::Builder &b, SpirvShaderParameters &parameters, const SceGxmProgram &program) {
    // HACKY: We assume output size and format

    std::string frag_color_name = "out_color";
    const spv::Id frag_color_type = b.makeVectorType(b.makeFloatType(32), 4);
    const spv::Id frag_color_var = create_variable(b, parameters, frag_color_name, USSE::RegisterBank::OUTPUT, 4, frag_color_type);

    b.addDecoration(frag_color_var, spv::DecorationLocation, 0);

    parameters.outs.push({ frag_color_type, frag_color_var }, 4);
}

static SpirvShaderParameters create_parameters(spv::Builder &b, const SceGxmProgram &program, emu::SceGxmProgramType program_type) {
    SpirvShaderParameters spv_params = {};
    const SceGxmProgramParameter *const gxp_parameters = gxp::program_parameters(program);
    StructDeclContext param_struct = {};

    for (size_t i = 0; i < program.parameter_count; ++i) {
        const SceGxmProgramParameter &parameter = gxp_parameters[i];

        gxp::log_parameter(parameter);

        USSE::RegisterBank param_reg_type = USSE::RegisterBank::PRIMATTR;

        switch (parameter.category) {
        case SCE_GXM_PARAMETER_CATEGORY_UNIFORM:
            param_reg_type = USSE::RegisterBank::SECATTR;
        // fallthrough
        case SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE: {
            const std::string struct_name = gxp::parameter_struct_name(parameter);
            const bool is_struct_field = !struct_name.empty();
            const bool struct_decl_ended = !is_struct_field && !param_struct.empty() || (is_struct_field && !param_struct.empty() && param_struct.name != struct_name);

            if (struct_decl_ended)
                create_struct(b, spv_params, param_struct, program_type);

            spv::Id param_type = get_param_type(b, parameter);

            const bool is_uniform = param_reg_type == USSE::RegisterBank::SECATTR;
            const bool is_vertex_output = param_reg_type == USSE::RegisterBank::OUTPUT && program_type == emu::SceGxmProgramType::Vertex;
            const bool is_fragment_input = param_reg_type == USSE::RegisterBank::PRIMATTR && program_type == emu::SceGxmProgramType::Fragment;
            const bool can_be_interface_block = is_vertex_output || is_fragment_input;

            // TODO: I haven't seen uniforms in 'structs' anywhere and can't test atm, so for now let's
            //       not try to implement emitting structs or interface blocks (probably the former)
            //       for them. Look below for current workaround (won't work for all cases).
            //       Cg most likely supports them so we should support it too at some point.
            if (is_struct_field && is_uniform)
                LOG_WARN("Uniform structs not fully supported!");
            const bool can_be_struct = can_be_interface_block; // || is_uniform

            if (is_struct_field && can_be_struct) {
                const auto param_field_name = gxp::parameter_name(parameter);

                param_struct.name = struct_name;
                param_struct.field_ids.push_back(param_type);
                param_struct.field_names.push_back(param_field_name);
                param_struct.reg_type = param_reg_type;
                param_struct.is_interaface_block = can_be_interface_block;
            } else {
                std::string var_name;

                if (is_uniform) {
                    // TODO: Hacky, ignores struct name/array index, uniforms names could collide if:
                    //           1) a global uniform is named the same as a struct field uniform
                    //           2) uniform struct arrays are used
                    //       It should work for other cases though, since set_uniforms also uses gxp::parameter_name
                    //       To fix this properly we need to emit structs properly first (see comment above
                    //       param_struct_t) and change set_uniforms to use gxp::parameter_name_raw.
                    //       Or we could just flatten everything.
                    var_name = gxp::parameter_name(parameter);
                } else {
                    var_name = gxp::parameter_name_raw(parameter);

                    if (is_struct_field)
                        // flatten struct
                        std::replace(var_name.begin(), var_name.end(), '.', '_');
                }

                for (auto p = 0; p < parameter.array_size; ++p) {
                    std::string var_elem_name;
                    if (parameter.array_size == 1)
                        var_elem_name = var_name;
                    else
                        var_elem_name = fmt::format("{}_{}", var_name, p);
                    create_variable(b, spv_params, var_elem_name, param_reg_type, parameter.component_count, param_type);
                }
            }
            break;
        }
        case SCE_GXM_PARAMETER_CATEGORY_SAMPLER: {
            create_param_sampler(b, spv_params, parameter);
            break;
        }
        case SCE_GXM_PARAMETER_CATEGORY_AUXILIARY_SURFACE: {
            assert(parameter.component_count == 0);
            LOG_CRITICAL("auxiliary_surface used in shader");
            break;
        }
        case SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER: {
            assert(parameter.component_count == 0);
            LOG_CRITICAL("uniform_buffer used in shader");
            break;
        }
        default: {
            LOG_CRITICAL("Unknown parameter type used in shader.");
            break;
        }
        }
    }

    // Declarations ended with a struct, so it didn't get handled and we need to do it here
    if (!param_struct.empty())
        create_struct(b, spv_params, param_struct, program_type);

    if (program_type == emu::SceGxmProgramType::Vertex)
        create_vertex_outputs(b, spv_params, program);
    else if (program_type == emu::SceGxmProgramType::Fragment) {
        create_fragment_inputs(b, spv_params, program);
        create_fragment_output(b, spv_params, program);
    }

    // Create temp reg vars
    for (auto i = 0; i < program.temp_reg_count1; i++) {
        auto name = fmt::format("r{}", i);
        auto type = b.makeVectorType(b.makeFloatType(32), 4); // TODO: Figure out correct type
        create_variable(b, spv_params, name, USSE::RegisterBank::TEMP, 4, type);
    }

    // Create internal reg vars
    for (auto i = 0; i < 3; i++) {
        auto name = fmt::format("i{}", i);
        // TODO: these are actually 128 bits long
        auto type = b.makeVectorType(b.makeFloatType(32), 4); // TODO: Figure out correct type
        create_variable(b, spv_params, name, USSE::RegisterBank::FPINTERNAL, 16, type);
    }

    // If this is a non-native color fragment shader (uses configurable blending, doesn't write to color buffer directly):
    // Add extra dummy primary attributes that on hw would be patched by the shader patcher depending on blending
    // Instead, in this case we write to the color buffer directly and emulate configurable blending with OpenGL
    // TODO: Verify creation logic. Should we just check if there are _no_ PAs ? Or is the current approach correct?
    if (program_type == emu::Fragment && !program.is_native_color()) {
        const auto missing_primary_attrs = program.primary_reg_count - spv_params.ins.size();

        if (missing_primary_attrs > 2) {
            LOG_ERROR("missing primary attrs are > 2");
        } else if (missing_primary_attrs > 0) {
            const auto pa_type = b.makeVectorType(b.makeFloatType(32), missing_primary_attrs * 2);
            std::string pa_name = "pa0_blend";
            create_variable(b, spv_params, pa_name, USSE::RegisterBank::PRIMATTR, missing_primary_attrs * 2, pa_type); // TODO: * 2 is a hack because we don't yet support f16
        }
    }

    return spv_params;
}

static void generate_shader_body(spv::Builder &b, const SpirvShaderParameters &parameters, const SceGxmProgram &program) {
    usse::convert_gxp_usse_to_spirv(b, program, parameters);
}

static SpirvCode convert_gxp_to_spirv(const SceGxmProgram &program, const std::string &shader_hash, bool force_shader_debug) {
    SpirvCode spirv;

    emu::SceGxmProgramType program_type = program.get_type();

    spv::SpvBuildLogger spv_logger;
    spv::Builder b(SPV_VERSION, 0x1337 << 12, &spv_logger);
    b.setSourceFile(shader_hash);
    b.setEmitOpLines();
    b.addSourceExtension("gxp");
    b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

    // Capabilities
    b.addCapability(spv::Capability::CapabilityShader);

    SpirvShaderParameters parameters = create_parameters(b, program, program_type);

    std::string entry_point_name;
    spv::ExecutionModel execution_model;

    switch (program_type) {
    default:
        LOG_ERROR("Unknown GXM program type");
    // fallthrough
    case emu::Vertex:
        entry_point_name = "main_vs";
        execution_model = spv::ExecutionModelVertex;
        break;
    case emu::Fragment:
        entry_point_name = "main_fs";
        execution_model = spv::ExecutionModelFragment;
        break;
    }

    // Entry point
    spv::Function *spv_func_main = b.makeEntryPoint(entry_point_name.c_str());

    generate_shader_body(b, parameters, program);

    b.leaveFunction();

    // Execution modes
    if (program_type == emu::SceGxmProgramType::Fragment)
        b.addExecutionMode(spv_func_main, spv::ExecutionModeOriginLowerLeft);

    // Add entry point to Builder
    auto entry_point = b.addEntryPoint(execution_model, spv_func_main, entry_point_name.c_str());

    for (auto id : parameters.ins.get_vars())
        entry_point->addIdOperand(id.var_id);
    for (auto id : parameters.outs.get_vars())
        entry_point->addIdOperand(id.var_id);

    auto spirv_log = spv_logger.getAllMessages();
    if (!spirv_log.empty())
        LOG_ERROR("SPIR-V Error:\n{}", spirv_log);

    b.dump(spirv);

    if (LOG_SHADER_DEBUG || force_shader_debug) {
        dump_spirv_disasm(b);
    }

    return spirv;
}

static std::string convert_spirv_to_glsl(SpirvCode spirv_binary) {
    spirv_cross::CompilerGLSL glsl(std::move(spirv_binary));

    spirv_cross::CompilerGLSL::Options options;
    options.version = 410;
    options.es = false;
    options.enable_420pack_extension = true;
    // TODO: this might be needed in the future
    //options.vertex.flip_vert_y = true;

    glsl.set_common_options(options);

    // Compile to GLSL, ready to give to GL driver.
    std::string source = glsl.compile();
    return source;
}

// ***********************
// * Functions (utility) *
// ***********************

void dump_spirv_disasm(const spv::Builder &b) {
    std::vector<uint32_t> spirv;
    std::stringstream spirv_disasm;
    b.dump(spirv);
    spv::Disassemble(spirv_disasm, spirv);
    LOG_DEBUG("SPIR-V Disassembly:\n{}", spirv_disasm.str());
}

// ***************************
// * Functions (exposed API) *
// ***************************

std::string convert_gxp_to_glsl(const SceGxmProgram &program, const std::string &shader_name, bool force_shader_debug) {
    std::vector<uint32_t> spirv_binary = convert_gxp_to_spirv(program, shader_name, force_shader_debug);

    const auto source = convert_spirv_to_glsl(spirv_binary);

    if (LOG_SHADER_DEBUG || force_shader_debug)
        LOG_DEBUG("Generated GLSL:\n{}", source);

    return source;
}

void convert_gxp_to_glsl_from_filepath(const std::string &shader_filepath_str) {
    const fs::path shader_filepath{ shader_filepath_str };
    fs::ifstream gxp_stream(shader_filepath, std::ios::binary);

    if (!gxp_stream.is_open())
        return;

    const auto gxp_file_size = fs::file_size(shader_filepath);
    auto gxp_program = static_cast<SceGxmProgram *>(calloc(gxp_file_size, 1));

    gxp_stream.read(reinterpret_cast<char *>(gxp_program), gxp_file_size);

    convert_gxp_to_glsl(*gxp_program, shader_filepath.filename().string(), true);

    free(gxp_program);
}

} // namespace shader
