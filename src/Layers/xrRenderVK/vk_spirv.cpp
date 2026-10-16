// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// SPIR-V of xrVulkan: the shader container, the DXC arguments, reflection of the modules DXC writes and
// the patching that makes them match the D3D11 binding model the engine speaks.
//
// Bindings. The engine binds by D3D11 register (its constant tables come from the DXIL reflection). DXC
// numbers SPIR-V bindings on its own, so every resource variable is renumbered from the container's table
// (HLSL name -> class + register): binding = class base + register, descriptor set = the stage's.
// Stage I/O. DXC gives locations in declaration order per stage; a vertex shader and the pixel shader it
// feeds may declare their structs differently. Inputs are matched by semantic at pipeline creation
// (RelinkInputs), like D3D11 matches signatures.
#include "vk_layer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace xrvk
{
namespace
{
// ---- SPIR-V constants (the subset the layer reads) ----------------------------------------------

constexpr u32 kMagic = 0x07230203;
enum Op : u32
{
    OpSource = 3,
    OpName = 5,
    OpMemberName = 6,
    OpExtension = 10,
    OpCapability = 17,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpTypeAccelerationStructureKHR = 5341,
    OpExecutionModeId = 331,
};
enum Decoration : u32
{
    DecBlock = 2,
    DecBufferBlock = 3,
    DecBuiltIn = 11,
    DecPatch = 15,
    DecSample = 17,
    DecLocation = 30,
    DecBinding = 33,
    DecDescriptorSet = 34,
};
enum StorageClass : u32
{
    ScUniformConstant = 0,
    ScInput = 1,
    ScUniform = 2,
    ScOutput = 3,
    ScStorageBuffer = 12,
};
enum ExecutionModel : u32
{
    EmVertex = 0,
    EmTessControl = 1,
    EmTessEval = 2,
    EmGeometry = 3,
    EmFragment = 4,
    EmCompute = 5,
    EmTask = 5364,
    EmMesh = 5365,
};
constexpr u32 kBuiltInSampleId = 18;
constexpr u32 kModeLocalSize = 17;
constexpr u32 kModeLocalSizeId = 38;

struct Decorations
{
    u32 setWord = 0;      // word index of the DescriptorSet literal (0 = none)
    u32 bindingWord = 0;  // word index of the Binding literal
    u32 locationWord = 0; // word index of the Location literal
    u32 location = ~0u;
    u32 builtIn = ~0u;
    bool block = false, bufferBlock = false, patch = false, sample = false;
};

struct TypeInfo
{
    u32 op = 0;
    u32 word = 0; // index of the instruction's first word
};

std::string Upper(std::string text)
{
    for (char& c : text)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// "TEXCOORD12" -> ("TEXCOORD", 12); a semantic without digits has index 0.
void SplitSemantic(const std::string& full, std::string& name, u32& index)
{
    size_t end = full.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(full[end - 1])))
        --end;
    name = Upper(full.substr(0, end));
    index = end < full.size() ? u32(std::strtoul(full.c_str() + end, nullptr, 10)) : 0;
}

u32 BaseOf(u8 cls)
{
    switch (cls)
    {
    case kClassCB: return kBindingCB;
    case kClassSRV: return kBindingSRV;
    case kClassSampler: return kBindingSampler;
    case kClassUAV: return kBindingUAV;
    case kClassCounter: return kBindingCounter;
    default: return kBindingLimit;
    }
}

u32 SlotLimit(u8 cls)
{
    switch (cls)
    {
    case kClassCB: return kMaxCBs;
    case kClassSRV: return kMaxSRVs;
    case kClassSampler: return kMaxSamplers;
    case kClassUAV:
    case kClassCounter: return kMaxUAVs;
    default: return 0;
    }
}

// The DXC arguments of the layer's compiles; the shifts keep explicit registers apart before the layer
// renumbers everything from the binding table.
const wchar_t* const kSpirvArgs[] = {
    L"-spirv",
    L"-fspv-target-env=vulkan1.3",
    // cbuffer and structured buffer packing exactly as in DXIL: the engine writes constants at the DXIL
    // reflection's offsets.
    L"-fvk-use-dx-layout",
    // SV_VertexID / SV_InstanceID exclude the base vertex / instance as in D3D.
    L"-fvk-support-nonzero-base-vertex",
    L"-fvk-support-nonzero-base-instance",
    // SV_Position.w in pixel shaders is w, not 1/w as FragCoord.w.
    L"-fvk-use-dx-position-w",
    L"-fvk-b-shift", L"0", L"0",
    L"-fvk-t-shift", L"16", L"0",
    L"-fvk-s-shift", L"48", L"0",
    L"-fvk-u-shift", L"64", L"0",
    L"-Wno-vk-ignored-features",
    L"-Wno-vk-emulated-features",
};
} // namespace

const wchar_t* const* SpirvCompileArgs(UINT& count)
{
    count = UINT(std::size(kSpirvArgs));
    return kSpirvArgs;
}

// ============================================================================================
// Container
// ============================================================================================

bool ParseShaderContainer(const void* blob, size_t size, ShaderContainerView& out)
{
    out = {};
    const u8* bytes = static_cast<const u8*>(blob);
    if (!bytes || size < 20)
        return false;
    u32 header[5];
    std::memcpy(header, bytes, sizeof(header));
    if (header[0] != kShaderContainerMagic || header[1] != kShaderContainerVersion)
        return false;
    const size_t dxilSize = header[2];
    const size_t spirvSize = header[3];
    const size_t bindingCount = header[4];
    const size_t dxilPadded = (dxilSize + 3) & ~size_t(3);
    const size_t total = 20 + dxilPadded + spirvSize + bindingCount * sizeof(ShaderBinding);
    if (total > size || spirvSize % 4 || !spirvSize)
        return false;
    out.dxil = dxilSize ? bytes + 20 : nullptr;
    out.dxilSize = dxilSize;
    out.spirv = reinterpret_cast<const uint32_t*>(bytes + 20 + dxilPadded);
    out.spirvWords = spirvSize / 4;
    out.bindings = reinterpret_cast<const ShaderBinding*>(bytes + 20 + dxilPadded + spirvSize);
    out.bindingCount = bindingCount;
    return true;
}

void BuildShaderContainer(const void* dxil, size_t dxilSize, const void* spirv, size_t spirvSize,
    const ShaderBinding* bindings, size_t bindingCount, std::vector<unsigned char>& out)
{
    const size_t dxilPadded = (dxilSize + 3) & ~size_t(3);
    const size_t spirvPadded = (spirvSize + 3) & ~size_t(3);
    out.assign(20 + dxilPadded + spirvPadded + bindingCount * sizeof(ShaderBinding), 0);
    const u32 header[5] = {kShaderContainerMagic, kShaderContainerVersion, u32(dxilSize), u32(spirvPadded),
        u32(bindingCount)};
    std::memcpy(out.data(), header, sizeof(header));
    if (dxilSize)
        std::memcpy(out.data() + 20, dxil, dxilSize);
    std::memcpy(out.data() + 20 + dxilPadded, spirv, spirvSize);
    if (bindingCount)
        std::memcpy(out.data() + 20 + dxilPadded + spirvPadded, bindings, bindingCount * sizeof(ShaderBinding));
}

// ============================================================================================
// Reflection and patching
// ============================================================================================

bool PrepareSpirv(const u32* words, size_t count, Stage stage, const ShaderBinding* table, size_t tableCount,
    std::vector<u32>& out, ShaderInterface& iface, std::string& entry, bool& mesh, std::string& error)
{
    iface = {};
    mesh = false;
    if (!words || count < 5 || words[0] != kMagic)
    {
        error = "not SPIR-V";
        return false;
    }
    out.assign(words, words + count);

    std::unordered_map<u32, std::string> names;
    std::unordered_map<u32, Decorations> decorations;
    std::unordered_map<u32, TypeInfo> types;
    std::unordered_map<u32, u32> constants;
    struct Variable
    {
        u32 id, type, storage;
    };
    std::vector<Variable> variables;
    u32 entryModel = ~0u;
    u32 entryId = 0;
    std::vector<u32> localSizeIds;

    // ---- pass 1: collect ----
    for (size_t i = 5; i < count;)
    {
        const u32 length = out[i] >> 16;
        const u32 op = out[i] & 0xFFFF;
        if (!length || i + length > count)
        {
            error = "malformed SPIR-V";
            return false;
        }
        switch (op)
        {
        case OpName:
            if (length >= 3)
                names[out[i + 1]] = reinterpret_cast<const char*>(&out[i + 2]);
            break;
        case OpEntryPoint:
            if (entryModel == ~0u && length >= 4)
            {
                entryModel = out[i + 1];
                entryId = out[i + 2];
                entry = reinterpret_cast<const char*>(&out[i + 3]);
            }
            break;
        case OpExecutionMode:
            if (length >= 6 && out[i + 1] == entryId && out[i + 2] == kModeLocalSize)
            {
                iface.localSize[0] = out[i + 3];
                iface.localSize[1] = out[i + 4];
                iface.localSize[2] = out[i + 5];
            }
            break;
        case OpExecutionModeId:
            if (length >= 6 && out[i + 1] == entryId && out[i + 2] == kModeLocalSizeId)
                localSizeIds = {out[i + 3], out[i + 4], out[i + 5]};
            break;
        case OpDecorate:
            if (length >= 3)
            {
                Decorations& d = decorations[out[i + 1]];
                const u32 dec = out[i + 2];
                if (dec == DecDescriptorSet && length >= 4)
                    d.setWord = u32(i + 3);
                else if (dec == DecBinding && length >= 4)
                    d.bindingWord = u32(i + 3);
                else if (dec == DecLocation && length >= 4)
                {
                    d.locationWord = u32(i + 3);
                    d.location = out[i + 3];
                }
                else if (dec == DecBuiltIn && length >= 4)
                    d.builtIn = out[i + 3];
                else if (dec == DecBlock)
                    d.block = true;
                else if (dec == DecBufferBlock)
                    d.bufferBlock = true;
                else if (dec == DecPatch)
                    d.patch = true;
                else if (dec == DecSample)
                    d.sample = true;
            }
            break;
        case OpTypeInt:
        case OpTypeFloat:
        case OpTypeVector:
        case OpTypeMatrix:
        case OpTypeImage:
        case OpTypeSampler:
        case OpTypeSampledImage:
        case OpTypeArray:
        case OpTypeRuntimeArray:
        case OpTypeStruct:
        case OpTypePointer:
        case OpTypeAccelerationStructureKHR:
            if (length >= 2)
                types[out[i + 1]] = {op, u32(i)};
            break;
        case OpConstant:
            if (length >= 4)
                constants[out[i + 2]] = out[i + 3];
            break;
        case OpVariable:
            if (length >= 4)
                variables.push_back({out[i + 2], out[i + 1], out[i + 3]});
            break;
        default:
            break;
        }
        i += length;
    }
    if (localSizeIds.size() == 3)
    {
        for (u32 c = 0; c < 3; ++c)
        {
            const auto it = constants.find(localSizeIds[c]);
            if (it != constants.end())
                iface.localSize[c] = it->second;
        }
    }

    // ---- stage check ----
    u32 wanted = EmVertex;
    switch (stage)
    {
    case Stage::VS: wanted = entryModel == EmMesh ? EmMesh : EmVertex; break;
    case Stage::PS: wanted = EmFragment; break;
    case Stage::GS: wanted = entryModel == EmTask ? EmTask : EmGeometry; break;
    case Stage::HS: wanted = EmTessControl; break;
    case Stage::DS: wanted = EmTessEval; break;
    case Stage::CS: wanted = EmCompute; break;
    default: break;
    }
    if (entryModel != wanted)
    {
        error = "the module's entry point is for another stage";
        return false;
    }
    mesh = entryModel == EmMesh || entryModel == EmTask;

    auto typeOf = [&](u32 id) -> const TypeInfo* {
        const auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    };
    // Scalar facts of a (vector/matrix/array of) scalar type.
    auto scalarOf = [&](u32 id, u32& components, bool& integer, u32& locations) {
        components = 1;
        integer = false;
        locations = 1;
        for (int depth = 0; depth < 8; ++depth)
        {
            const TypeInfo* t = typeOf(id);
            if (!t)
                return;
            switch (t->op)
            {
            case OpTypeArray:
            {
                const auto c = constants.find(out[t->word + 3]);
                locations *= c != constants.end() ? c->second : 1;
                id = out[t->word + 2];
                break;
            }
            case OpTypeMatrix:
                locations *= out[t->word + 3];
                id = out[t->word + 2];
                break;
            case OpTypeVector:
                components = out[t->word + 3];
                id = out[t->word + 2];
                break;
            case OpTypeInt:
                integer = true;
                return;
            default:
                return;
            }
        }
    };

    // ---- resources ----
    const u32 set = SetOf(stage);
    u32 unknownBinding = kBindingLimit;
    auto lookup = [&](const std::string& name, u8& cls, u32& reg, u32& arraySize) {
        for (size_t t = 0; t < tableCount; ++t)
        {
            if (name == table[t].name)
            {
                cls = table[t].cls;
                reg = table[t].reg;
                arraySize = table[t].count ? table[t].count : 1;
                return true;
            }
        }
        return false;
    };

    for (const Variable& v : variables)
    {
        const TypeInfo* ptr = typeOf(v.type);
        if (!ptr || ptr->op != OpTypePointer)
            continue;
        const u32 storage = out[ptr->word + 2];
        u32 pointee = out[ptr->word + 3];
        Decorations& dec = decorations[v.id];

        if (storage == ScInput || storage == ScOutput)
        {
            if (dec.builtIn == kBuiltInSampleId)
                iface.sampleShading = true;
            if (dec.sample)
                iface.sampleShading = true;
            if (dec.location == ~0u || !dec.locationWord)
                continue;
            const auto nameIt = names.find(v.id);
            std::string varName = nameIt != names.end() ? nameIt->second : std::string();
            const char* prefix = storage == ScInput ? "in.var." : "out.var.";
            ShaderInterface::Var var;
            if (varName.rfind(prefix, 0) == 0)
                SplitSemantic(varName.substr(std::strlen(prefix)), var.semantic, var.index);
            else
                var.semantic = Upper(varName);
            var.location = dec.location;
            var.locationWord = dec.locationWord;
            var.patch = dec.patch;
            // Per-vertex arrays of the tessellation and geometry stages: the element is the variable.
            u32 element = pointee;
            const bool arrayed = !dec.patch &&
                ((storage == ScInput && (stage == Stage::HS || stage == Stage::DS || stage == Stage::GS)) ||
                    (storage == ScOutput && (stage == Stage::HS || (stage == Stage::VS && mesh))));
            if (arrayed)
            {
                const TypeInfo* t = typeOf(element);
                if (t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray))
                    element = out[t->word + 2];
            }
            scalarOf(element, var.components, var.integer, var.locations);
            (storage == ScInput ? iface.inputs : iface.outputs).push_back(std::move(var));
            continue;
        }

        if (storage != ScUniformConstant && storage != ScUniform && storage != ScStorageBuffer)
            continue;
        if (!dec.bindingWord)
            continue;

        // Descriptor count of arrays.
        u32 arrayCount = 1;
        if (const TypeInfo* t = typeOf(pointee); t && t->op == OpTypeArray)
        {
            const auto c = constants.find(out[t->word + 3]);
            arrayCount = c != constants.end() ? c->second : 1;
            pointee = out[t->word + 2];
        }
        else if (t && t->op == OpTypeRuntimeArray)
        {
            error = "unbounded resource arrays are not supported on the D3D11 path";
            return false;
        }

        SetBinding b;
        b.viewType = 0xFF;
        const TypeInfo* t = typeOf(pointee);
        if (!t)
            continue;
        switch (t->op)
        {
        case OpTypeSampler:
            b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case OpTypeImage:
        {
            const u32 dim = out[t->word + 3];
            const u32 arrayed = out[t->word + 5];
            const u32 ms = out[t->word + 6];
            const u32 sampled = out[t->word + 7];
            if (dim == 5) // Buffer
                b.type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else
            {
                b.type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                VkImageViewType view = VK_IMAGE_VIEW_TYPE_2D;
                switch (dim)
                {
                case 0: view = arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D; break;
                case 1: view = arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; break;
                case 2: view = VK_IMAGE_VIEW_TYPE_3D; break;
                case 3: view = arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE; break;
                default: break;
                }
                (void)ms;
                b.viewType = u8(view);
            }
            break;
        }
        case OpTypeSampledImage:
            error = "combined image samplers are not used by the D3D11 path";
            return false;
        case OpTypeAccelerationStructureKHR:
            b.type = kTypeAccelerationStructure;
            break;
        case OpTypeStruct:
        {
            const Decorations& sd = decorations[pointee];
            if (storage == ScUniform && sd.block)
                b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            else if (storage == ScStorageBuffer || (storage == ScUniform && sd.bufferBlock))
                b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else
                continue;
            break;
        }
        default:
            continue;
        }

        const auto nameIt = names.find(v.id);
        const std::string varName = nameIt != names.end() ? nameIt->second : std::string();
        u8 cls = kClassUnknown;
        u32 reg = 0, tableCountOf = 1;
        constexpr char kCounterPrefix[] = "counter.var.";
        if (varName.rfind(kCounterPrefix, 0) == 0)
        {
            if (lookup(varName.substr(sizeof(kCounterPrefix) - 1), cls, reg, tableCountOf) && cls == kClassUAV)
                cls = kClassCounter;
            else
                cls = kClassUnknown;
        }
        else if (!lookup(varName, cls, reg, tableCountOf))
            cls = kClassUnknown;

        u32 binding;
        if (cls == kClassUnknown || reg + arrayCount > SlotLimit(cls))
        {
            static constexpr char kFormat[] = "~ a shader resource has no register in the table or one past the layer's "
                                              "limit (%s): it reads as null";
            LogOnce(&kFormat, kFormat, varName.c_str());
            cls = kClassUnknown;
            binding = unknownBinding++;
            reg = 0;
        }
        else
            binding = BaseOf(cls) + reg;
        if (binding > 255)
        {
            error = "too many unbound resources";
            return false;
        }
        out[dec.bindingWord] = binding;
        if (dec.setWord)
            out[dec.setWord] = set;
        b.binding = u8(binding);
        b.cls = cls;
        b.slot = u8(reg);
        b.count = u8(arrayCount);
        iface.bindings.push_back(b);
        for (u32 e = 0; e < arrayCount && cls != kClassUnknown; ++e)
        {
            switch (cls)
            {
            case kClassCB: iface.cbMask |= u16(1u << (reg + e)); break;
            case kClassSRV: iface.srvMask |= 1u << (reg + e); break;
            case kClassSampler: iface.samplerMask |= u16(1u << (reg + e)); break;
            case kClassUAV: iface.uavMask |= u8(1u << (reg + e)); break;
            default: break;
            }
        }
    }
    std::sort(iface.bindings.begin(), iface.bindings.end(),
        [](const SetBinding& a, const SetBinding& b) { return a.binding < b.binding; });
    for (size_t i = 1; i < iface.bindings.size(); ++i)
    {
        if (iface.bindings[i].binding == iface.bindings[i - 1].binding)
        {
            error = "two resources share a register";
            return false;
        }
    }
    return true;
}

bool RelinkInputs(const Shader& producer, const Shader& consumer, std::vector<u32>& out)
{
    const auto& outputs = producer.iface.outputs;
    const auto& inputs = consumer.iface.inputs;
    bool differs = false;
    u32 nextFree = 0;
    for (const ShaderInterface::Var& o : outputs)
        nextFree = (std::max)(nextFree, o.location + o.locations);
    std::vector<std::pair<u32, u32>> moves; // (word, location)
    moves.reserve(inputs.size());
    for (const ShaderInterface::Var& in : inputs)
    {
        const ShaderInterface::Var* match = nullptr;
        for (const ShaderInterface::Var& o : outputs)
        {
            if (o.patch == in.patch && o.index == in.index && o.semantic == in.semantic)
            {
                match = &o;
                break;
            }
        }
        u32 location;
        if (match)
            location = match->location;
        else
        {
            // D3D11 reads an unwritten input as undefined: give it a location nothing writes.
            location = nextFree;
            nextFree += in.locations;
        }
        if (location != in.location)
            differs = true;
        moves.push_back({in.locationWord, location});
    }
    if (!differs)
        return false;
    out = consumer.spirv;
    for (const auto& [word, location] : moves)
    {
        if (word < out.size())
            out[word] = location;
    }
    return true;
}

std::string MissingCapabilities(Device* device, const std::vector<u32>& spirv)
{
    const Device::ShaderFeatures& f = device->shaderFeatures;
    std::string missing;
    auto need = [&](bool have, const char* name) {
        if (have)
            return;
        if (!missing.empty())
            missing += ", ";
        missing += name;
    };
    for (size_t i = 5; i < spirv.size();)
    {
        const u32 length = spirv[i] >> 16;
        const u32 op = spirv[i] & 0xFFFF;
        if (!length)
            break;
        if (op == OpCapability && length >= 2)
        {
            switch (spirv[i + 1])
            {
            case 2: need(f.geometry, "Geometry"); break;
            case 3: need(f.tessellation, "Tessellation"); break;
            case 9: need(f.float16, "Float16"); break;
            case 10: need(f.float64, "Float64"); break;
            case 11: need(f.int64, "Int64"); break;
            case 12: need(f.int64Atomics, "Int64Atomics"); break;
            case 22: need(f.int16, "Int16"); break;
            case 32: need(f.clipDistance, "ClipDistance"); break;
            case 33: need(f.cullDistance, "CullDistance"); break;
            case 34:
            case 45: need(f.cubeArray, "ImageCubeArray"); break;
            case 35: need(f.sampleRate, "SampleRateShading"); break;
            case 39: need(f.int8, "Int8"); break;
            case 42: need(f.minLod, "MinLod"); break;
            case 49: need(f.storageExtended, "StorageImageExtendedFormats"); break;
            case 54: need(f.geometryStreams, "GeometryStreams"); break;
            case 55: need(f.readWithoutFormat, "StorageImageReadWithoutFormat"); break;
            case 56: need(f.writeWithoutFormat, "StorageImageWriteWithoutFormat"); break;
            case 57: need(f.multiViewport, "MultiViewport"); break;
            case 69: need(f.outputLayer, "ShaderLayer"); break;
            case 70: need(f.outputViewport, "ShaderViewportIndex"); break;
            case 4427: need(f.drawParameters, "DrawParameters"); break;
            case 4472: need(f.rayQuery, "RayQuery"); break;
            case 5013: need(f.stencilExport, "StencilExport"); break;
            case 5254: need(f.viewportLayer, "ShaderViewportIndexLayer"); break;
            case 5265: need(f.fullyCovered, "FragmentFullyCovered"); break;
            case 5283: need(f.mesh, "MeshShading"); break;
            case 5363:
            case 5372:
            case 5378: need(f.interlock, "FragmentShaderInterlock"); break;
            default: break;
            }
        }
        // Capabilities come first in a module: the entry point ends them.
        else if (op == OpEntryPoint)
            break;
        i += length;
    }
    return missing;
}
} // namespace xrvk
