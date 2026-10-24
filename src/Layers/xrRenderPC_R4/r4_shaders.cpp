#include "stdafx.h"
#include "r2.h"
#include "Layers/xrRender/ShaderResourceTraits.h"
#include "xrCore/FileCRC32.h"
#include "da_dxc.h"
#include "xrEngine/da_watchdog.h"

#ifdef XR_NATIVE
#include "Layers/xrRenderDX11/dx11Native.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
void CRender::addShaderOption(const char* name, const char* value)
{
    D3D_SHADER_MACRO macro = {name, value};
    m_ShaderOptions.push_back(macro);
}

// The shader's reflection: R5 and R6 through xrD3D12 (DXBC, DXIL and the Vulkan container's DXIL twin alike,
// D3D12 interfaces), R4 D3DReflect.
static HRESULT reflect_shader(const void* data, size_t size, ID3DShaderReflection** reflection)
{
#ifdef XR_NATIVE
    return xrnative::Reflect(data, size, IID_ID3DShaderReflection, reinterpret_cast<void**>(reflection));
#else
    return D3DReflect(data, size, IID_ID3DShaderReflection, reinterpret_cast<void**>(reflection));
#endif
}

template <typename T>
static HRESULT create_shader(DWORD const* buffer, size_t const buffer_size, ID3DShaderReflection* reflection,
    LPCSTR const file_name, T*& result, bool const dx9compatibility, bool const dxil)
{
    HRESULT _hr = ShaderTypeTraits<T>::CreateHWShader(buffer, buffer_size, result->sh);
    if (!SUCCEEDED(_hr))
    {
        Log("! Shader: ", file_name);
        Msg("! CreateHWShader hr == 0x%08x", _hr);
        return E_FAIL;
    }

    // The DirectX 12 and Vulkan layers name refused pipelines by these.
#if defined(DEBUG) || defined(XR_NATIVE)
    if (result->sh)
    {
        result->sh->SetPrivateData(WKPDID_D3DDebugObjectName, u32(xr_strlen(file_name)), file_name);
    }
#endif

    if (!reflection)
    {
        // The next attempt (FXC after DXC, a recompile after a stale cache entry) creates it anew.
        _RELEASE(result->sh);
        return E_FAIL;
    }

    result->dxil = dxil;
    result->constants.dx9compatibility = dx9compatibility;
    result->constants.dxil = dxil;
    // Parse constant table data
    result->constants.parse(reflection, ShaderTypeTraits<T>::GetShaderDest());
    return S_OK;
}

// `reflection_data`: DXC's reflection blob of a DXIL object; null for DXBC, which reflects itself.
static HRESULT create_shader(LPCSTR const pTarget, DWORD const* buffer, size_t const buffer_size,
    const void* reflection_data, size_t reflection_size, LPCSTR const file_name, void*& result, bool const disasm,
    bool const dx9compatibility, bool const dxil)
{
    ID3DShaderReflection* reflection = nullptr;
    const HRESULT reflected = reflection_data ? reflect_shader(reflection_data, reflection_size, &reflection) :
                                                reflect_shader(buffer, buffer_size, &reflection);
    if (FAILED(reflected) || !reflection)
    {
        Msg("! D3DReflectShader %s hr == 0x%08x", file_name, reflected);
        reflection = nullptr;
    }

    HRESULT _result = E_FAIL;
    pcstr extension = ".hlsl";
    switch (pTarget[0])
    {
    case 'p':
        extension = ".ps";
        _result = create_shader(buffer, buffer_size, reflection, file_name, (SPS*&)result, dx9compatibility, dxil);
        break;
    // A mesh shader rides the vertex slot (the layer knows it by its program kind).
    case 'v':
    case 'm':
    {
        extension = pTarget[0] == 'm' ? ".ms" : ".vs";
        SVS* svs_result = (SVS*)result;
        _result = create_shader(buffer, buffer_size, reflection, file_name, svs_result, dx9compatibility, dxil);
        if (SUCCEEDED(_result))
        {
            //	Store input signature (need only for VS); DXIL carries one too (ISG1, empty for a mesh shader)
            // On Vulkan the signature is the container's DXIL twin's.
            const void* signature_source = buffer;
            size_t signature_size = buffer_size;
#ifdef XR_NATIVE
            xrnative::ShaderBytecode(signature_source, signature_size);
#endif
            ID3DBlob* pSignatureBlob;
            CHK_DX(D3DGetInputSignatureBlob(signature_source, signature_size, &pSignatureBlob));
            VERIFY(pSignatureBlob);

            svs_result->signature = RImplementation.Resources->_CreateInputSignature(pSignatureBlob);

            _RELEASE(pSignatureBlob);

            // Classify the inputs once per signature: only POSITION and system values means
            // the position-only fast geometry may stand in for the full vertex format.
            if (svs_result->signature)
            {
                bool position_only = true;
                D3D_SHADER_DESC shader_desc{};
                if (SUCCEEDED(reflection->GetDesc(&shader_desc)))
                {
                    for (UINT i = 0; i < shader_desc.InputParameters && position_only; ++i)
                    {
                        D3D_SIGNATURE_PARAMETER_DESC param{};
                        if (FAILED(reflection->GetInputParameterDesc(i, &param)) || !param.SemanticName)
                            continue;
                        if (param.SystemValueType != D3D_NAME_UNDEFINED)
                            continue;
                        if (xr_stricmp(param.SemanticName, "POSITION") != 0)
                            position_only = false;
                    }
                }
                svs_result->signature->position_only = position_only;
            }
        }
        break;
    }
    // An amplification shader rides the geometry slot.
    case 'g':
    case 'a':
        extension = pTarget[0] == 'a' ? ".as" : ".gs";
        _result = create_shader(buffer, buffer_size, reflection, file_name, (SGS*&)result, dx9compatibility, dxil);
        break;
    case 'c':
        extension = ".cs";
        _result = create_shader(buffer, buffer_size, reflection, file_name, (SCS*&)result, dx9compatibility, dxil);
        break;
    case 'h':
        extension = ".hs";
        _result = create_shader(buffer, buffer_size, reflection, file_name, (SHS*&)result, dx9compatibility, dxil);
        break;
    case 'd':
        extension = ".ds";
        _result = create_shader(buffer, buffer_size, reflection, file_name, (SDS*&)result, dx9compatibility, dxil);
        break;
    default:
        NODEFAULT;
    }
    _RELEASE(reflection);

    if (disasm)
    {
        string_path dname;
        strconcat(sizeof(dname), dname, "disasm" DELIMITER, file_name, extension, dxil ? ".dxil.txt" : "");
#ifdef XR_NATIVE
        std::string text;
        const void* listing_source = buffer;
        size_t listing_size = buffer_size;
        xrnative::ShaderBytecode(listing_source, listing_size);
        if (SUCCEEDED(xrd3d12::Disassemble(listing_source, listing_size, text)) && !text.empty())
        {
            IWriter* W = FS.w_open("$app_data_root$", dname);
            W->w(text.data(), text.size());
            FS.w_close(W);
        }
#else
        ID3DBlob* disasm_blob = nullptr;
        D3DDisassemble(buffer, buffer_size, FALSE, 0, &disasm_blob);
        if (!disasm_blob)
            return _result;

        IWriter* W = FS.w_open("$app_data_root$", dname);
        W->w(disasm_blob->GetBufferPointer(), disasm_blob->GetBufferSize());
        FS.w_close(W);
        _RELEASE(disasm_blob);
#endif
    }

    return _result;
}

class includer : public ID3DInclude
{
public:
    HRESULT __stdcall Open(
        D3D10_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes)
    {
        string_path pname;
        strconcat(pname, RImplementation.getShaderPath(), pFileName);
        IReader* R = FS.r_open("$game_shaders$", pname);
        if (nullptr == R)
        {
            // possibly in shared directory or somewhere else - open directly
            R = FS.r_open("$game_shaders$", pFileName);
            if (nullptr == R)
                return E_FAIL;
        }

        // duplicate and zero-terminate
        const size_t size = R->length();
        u8* data = xr_alloc<u8>(size + 1);
        CopyMemory(data, R->pointer(), size);
        data[size] = 0;
        FS.r_close(R);

        *ppData = data;
        *pBytes = UINT(size);
        return D3D_OK;
    }
    HRESULT __stdcall Close(LPCVOID pData)
    {
        auto mutableData = const_cast<LPVOID>(pData);
        xr_free(mutableData);
        return D3D_OK;
    }
};

class shader_name_holder
{
    size_t pos{};
    string_path name;

public:
    void append(cpcstr string)
    {
        // string_path is finite and the option list is not - clamp instead of writing past it.
        const size_t size = xr_strlen(string);
        for (size_t i = 0; i < size && pos < sizeof(name) - 1; ++i)
        {
            name[pos] = string[i];
            ++pos;
        }
    }

    void append(u32 value)
    {
        if (pos < sizeof(name) - 1)
        {
            name[pos] = '0' + char(value); // NOLINT
            ++pos;
        }
    }

    void finish()
    {
        name[pos] = '\0';
    }

    pcstr c_str() const { return name; }
};

class shader_options_holder
{
    size_t pos{};
    D3D_SHADER_MACRO m_options[128];

public:
    void add(const xr_vector<D3D_SHADER_MACRO>& macros)
    {
        for (auto macro : macros)
        {
            m_options[pos] = std::move(macro);
            ++pos;
        }
    }

    void add(cpcstr name, cpcstr value)
    {
        m_options[pos] = { name, value };
        ++pos;
    }

    void finish()
    {
        m_options[pos] = { nullptr, nullptr };
    }

    D3D_SHADER_MACRO* data() { return m_options; }
};

// The release's own copy of the cache (gamedata\shaders_cache_pack in the mod's archive, a QA sweep of the presets on
// every renderer, tools\package\build_shader_pack.ps1): read where the player's has no valid file. Every file
// carries its source's CRC with the includes, so one a shader edit outdated compiles as before. Empty with
// -no_shader_pack: the sweep that builds the pack compiles everything (the QA rig shares the live install's archive).
static void shader_pack_path(string_path& out, pcstr relative)
{
    static const bool disabled = !!strstr(Core.Params, "-no_shader_pack");
    out[0] = 0;
    if (disabled)
        return;
    string_path file;
    strconcat(sizeof(file), file, "shaders_cache_pack" DELIMITER, relative);
    FS.update_path(out, "$game_data$", file);
}

#ifdef XR_NATIVE
#ifdef XR_VULKAN
constexpr char k_native_cache_root[] = "r6_spirv";
#else
constexpr char k_native_cache_root[] = "r5_dxil";
#endif
// An engine shader through DXC with its r5_dxil cache (dxc.md 3.3, 3.5); on Vulkan a container of DXIL and
// SPIR-V in r6_spirv.
static HRESULT compile_dxil(pcstr name, pcstr extension, pcstr sh_name, IReader* fs, u32 fileCrc, pcstr entry,
    pcstr target, const D3D_SHADER_MACRO* options, bool disasm, void*& result)
{
    // The name the layer reports refused pipelines by, the cache file and its copy in the release's pack.
    string_path shader_name, cache_file, pack_file;
    {
        strconcat(sizeof(shader_name), shader_name, k_native_cache_root, DELIMITER, name, ".", extension, DELIMITER,
            sh_name);
        string_path relative, file;
        strconcat(sizeof(relative), relative, da_dxc_cache_dir(), name, ".", extension, DELIMITER, sh_name);
        strconcat(sizeof(file), file, "shaders_cache_grizzly" DELIMITER, relative);
        FS.update_path(cache_file, "$app_data_root$", file);
        shader_pack_path(pack_file, relative);
    }

    // Refused this session (DXC or the layer): straight to FXC, no cache probe, no recompile.
    if (da_dxc_refused(cache_file, fileCrc))
        return E_FAIL;

    xr_vector<u8> object, reflection;
    if (da_dxc_cache_load(cache_file, fileCrc, object, reflection) ||
        (pack_file[0] && da_dxc_cache_load(pack_file, fileCrc, object, reflection)))
    {
        const HRESULT hr = create_shader(target, reinterpret_cast<const DWORD*>(object.data()), object.size(),
            reflection.data(), reflection.size(), shader_name, result, disasm, false, true);
        if (SUCCEEDED(hr))
            return hr;
    }

    string_path source_name;
    strconcat(sizeof(source_name), source_name, name, ".", extension);
    includer Includer;
    xr_string error;
    if (!da_dxc_compile_engine(source_name, fs->pointer(), fs->length(), options, &Includer, entry, target, object,
            reflection, error))
    {
        da_dxc_note_refusal(cache_file, fileCrc);
        Msg("! [dxc] %s: %s", shader_name, error.c_str());
        for (const D3D_SHADER_MACRO* m = options; m && m->Name; ++m)
            Msg("!   with define: %s=%s", m->Name, m->Definition ? m->Definition : "");
        return E_FAIL;
    }
    // Only an object the layer accepted is cached; a rejected one is refused like a compile error.
    const HRESULT hr = create_shader(target, reinterpret_cast<const DWORD*>(object.data()), object.size(),
        reflection.data(), reflection.size(), shader_name, result, disasm, false, true);
    if (FAILED(hr))
    {
        da_dxc_note_refusal(cache_file, fileCrc);
        return hr;
    }
    da_dxc_cache_store(cache_file, fileCrc, object, reflection);
    return hr;
}
#endif

HRESULT CRender::shader_compile(pcstr name, IReader* fs, pcstr pFunctionName,
    pcstr pTarget, u32 Flags, void*& result)
{
    // A cold cache compiles for a long while on the main thread (a renderer restart, the first start): it is alive.
    da_watchdog_beat();
    shader_options_holder options;
    shader_name_holder sh_name;

    // Don't move these variables to lower scope!
    string32 c_smap;
    string32 c_gloss;
    string32 c_sun_shafts;
    string32 c_ssao;
    string32 c_sun_quality;
    char c_msaa_samples[2];
    char c_msaa_current_sample[2];

    // options:
    const auto appendShaderOption = [&](u32 option, cpcstr macro, cpcstr value)
    {
        if (option)
            options.add(macro, value);

        sh_name.append(option);
    };

    // External defines
    options.add(m_ShaderOptions);
    // The external option set must reach the cache KEY too, or adding a define serves stale
    // bytecode compiled without it. crc32 of name=value pairs, hex-appended (the built-in
    // appendShaderOption path already contributes digits; this covers everything else).
    {
        u32 opt_crc = 0;
        for (const auto& macro : m_ShaderOptions)
        {
            if (!macro.Name)
                break;
            opt_crc = crc32(macro.Name, u32(xr_strlen(macro.Name)), opt_crc);
            if (macro.Definition)
                opt_crc = crc32(macro.Definition, u32(xr_strlen(macro.Definition)), opt_crc);
        }
        if (opt_crc)
        {
            string32 opt_hex;
            xr_sprintf(opt_hex, "%08x", opt_crc);
            sh_name.append(static_cast<cpcstr>(opt_hex));
        }
    }

    // Shadow map size
    {
        xr_sprintf(c_smap, "%u", m_SMAPSize);
        options.add("SMAP_size", c_smap);
        sh_name.append(c_smap);
    }

    // FP16 Filter
    appendShaderOption(o.fp16_filter, "FP16_FILTER", "1");

    // FP16 Blend
    appendShaderOption(o.fp16_blend, "FP16_BLEND", "1");

    // HW smap
    appendShaderOption(o.HW_smap, "USE_HWSMAP", "1");

    // HW smap PCF
    appendShaderOption(o.HW_smap_PCF, "USE_HWSMAP_PCF", "1");

    // Fetch4
    appendShaderOption(o.HW_smap_FETCH4, "USE_FETCH4", "1");

    // SJitter
    appendShaderOption(o.sjitter, "USE_SJITTER", "1");

    // Branching
    appendShaderOption(HW.Caps.raster_major >= 3, "USE_BRANCHING", "1");

    // Vertex texture fetch
    appendShaderOption(HW.Caps.geometry.bVTF, "USE_VTF", "1");

    // Tshadows
    appendShaderOption(o.Tshadows, "USE_TSHADOWS", "1");

    // Motion blur

    // Sun filter
    appendShaderOption(o.sunfilter, "USE_SUNFILTER", "1");

    // Static sun on R2 and higher
    appendShaderOption(o.sunstatic, "USE_R2_STATIC_SUN", "1");

    // Force gloss
    {
        xr_sprintf(c_gloss, "%f", o.forcegloss_v);
        appendShaderOption(o.forcegloss, "FORCE_GLOSS", c_gloss);
    }

    // Force skinw
    appendShaderOption(o.forceskinw, "SKIN_COLOR", "1");

    // SSAO Blur
    appendShaderOption(o.ssao_blur_on, "USE_SSAO_BLUR", "1");

    // SSAO HDAO
    if (o.ssao_hdao)
    {
        options.add("HDAO", "1");
        sh_name.append(static_cast<u32>(1)); // HDAO on
        sh_name.append(static_cast<u32>(0)); // HBAO off
        sh_name.append(static_cast<u32>(0)); // Half data off
    }
    else // SSAO HBAO
    {
        sh_name.append(static_cast<u32>(0)); // HDAO off
        sh_name.append(o.ssao_hbao);         // HBAO on/off
        sh_name.append(o.ssao_half_data);    // Half data on/off

        if (o.ssao_hbao)
        {
            if (o.ssao_half_data)
                options.add("SSAO_OPT_DATA", "2");
            else
                options.add("SSAO_OPT_DATA", "1");

            if (o.hbao_vectorized)
                options.add("VECTORIZED_CODE", "1");

            options.add("USE_HBAO", "1");
        }
    }

    // GTAO: the AO term is pre-rendered and guided-filtered into $user$ssao_temp by
    // phase_gtao; USE_GTAO switches combine_1 from the inline calc to sampling s_occ.
    // Not under MSAA - the pre-pass reads the deferred position as a plain 2D surface.
    appendShaderOption(!o.msaa && ps_r_ssao_mode == ssao_mode_gtao, "USE_GTAO", "1");

    // skinning
    // SKIN_NONE
    appendShaderOption(m_skinning < 0, "SKIN_NONE", "1");

    // SKIN_0
    appendShaderOption(0 == m_skinning, "SKIN_0", "1");

    // SKIN_1
    appendShaderOption(1 == m_skinning, "SKIN_1", "1");

    // SKIN_2
    appendShaderOption(2 == m_skinning, "SKIN_2", "1");

    // SKIN_3
    appendShaderOption(3 == m_skinning, "SKIN_3", "1");

    // SKIN_4
    appendShaderOption(4 == m_skinning, "SKIN_4", "1");

    //	Igor: need restart options
    // Soft water
    {
        const bool softWater = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_WATER);
        appendShaderOption(softWater, "USE_SOFT_WATER", "1");
    }

    // Soft particles
    {
        const bool useSoftParticles = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_PARTICLES);
        appendShaderOption(useSoftParticles, "USE_SOFT_PARTICLES", "1");
    }

    // Depth of field
    {
        const bool dof = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_DOF);
        appendShaderOption(dof, "USE_DOF", "1");
    }

    appendShaderOption(ps_r2_technicolor, "USE_TECHICOLOR", "1");
    appendShaderOption(ps_r2_vignette, "USE_VIGNETTE", "1");
    appendShaderOption(ps_r2_filmgrain, "USE_FILMGRAIN", "1");
    // The visor's dirt is a mask's and is switched per frame by whether a mask is up, so it is
    // always compiled: a define would cost a shader restart every time a mask goes on or off.
    appendShaderOption(RImplementation.o.advancedpp, "USE_LENS_DIRT", "1");
    appendShaderOption(ps_r2_lenswater, "USE_LENS_WATER", "1");
    // Water screen-space reflections. USE_REFLECTIONS gates the whole SSLR branch in the
    // water shader (the R2 engine defined it, R4 never did - "sky reflects but nothing else").
    // Driven by the existing r3_water_refl token: off drops the define entirely and the water
    // falls back to the plain cubemap at zero cost; SSR_QUALITY picks the march length in
    // ogse_reflections.h, and its value lands in the shader-cache name via sh_name.append.
    {
        appendShaderOption(ps_r_water_reflection ? 1 : 0, "USE_REFLECTIONS", "1");
        static string16 c_water_reflection;
        xr_sprintf(c_water_reflection, "%u", ps_r_water_reflection);
        appendShaderOption(ps_r_water_reflection, "SSR_QUALITY", c_water_reflection);
        // Dithers each pixel's ray start so the fixed-stride march breaks its stair-step
        // banding into fine noise instead of visible terraces along reflection edges.
        appendShaderOption(
            ps_r_water_reflection && ps_r2_ls_flags_ext.test(R3FLAGEXT_SSR_JITTER), "SSR_JITTER", "1");
    }
    appendShaderOption(RImplementation.o.advancedpp && ps_r2_sss_enable, "USE_SSHAFTS", "1");
    // No USE_FXAA: no shader reads it, and anti-aliasing switches live now (r__aa).

    // Sun shafts
    if (RImplementation.o.advancedpp && ps_r_sun_shafts)
    {
        xr_sprintf(c_sun_shafts, "%d", ps_r_sun_shafts);
        options.add("SUN_SHAFTS_QUALITY", c_sun_shafts);
        sh_name.append(ps_r_sun_shafts);
    }
    else
        sh_name.append(static_cast<u32>(0));

    if (RImplementation.o.advancedpp && ps_r_ssao)
    {
        xr_sprintf(c_ssao, "%d", ps_r_ssao);
        options.add("SSAO_QUALITY", c_ssao);
        sh_name.append(ps_r_ssao);
    }
    else
        sh_name.append(static_cast<u32>(0));

    // Sun quality
    if (RImplementation.o.advancedpp && ps_r_sun_quality)
    {
        xr_sprintf(c_sun_quality, "%d", ps_r_sun_quality);
        options.add("SUN_QUALITY", c_sun_quality);
        sh_name.append(ps_r_sun_quality);
    }
    else
        sh_name.append(static_cast<u32>(0));

    // Steep parallax
    {
        const bool steepParallax = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_STEEP_PARALLAX);
        appendShaderOption(steepParallax, "ALLOW_STEEPPARALLAX", "1");
    }

    // Geometry buffer optimization
    appendShaderOption(o.gbuffer_opt, "GBUFFER_OPTIMIZATION", "1");

    // Shader Model 4.1
    appendShaderOption(o.dx11_sm4_1, "SM_4_1", "1");

    // Shader Model 5.0
    appendShaderOption(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0, "SM_5", "1");

    // Double precision
    appendShaderOption(HW.DoublePrecisionFloatShaderOps, "DOUBLE_PRECISION", "1");

    // Extended doubles instructions
    appendShaderOption(HW.ExtendedDoublesShaderInstructions, "EXTENDED_DOUBLES", "1");

    // SAD4 intrinsic support
    appendShaderOption(HW.SAD4ShaderInstructions, "SAD4_SUPPORTED", "1");

    // Minmax SM
    appendShaderOption(o.minmax_sm, "USE_MINMAX_SM", "1");

    // Be carefull!!!!! this should be at the end to correctly generate
    // compiled shader name;
    // add a #define for DX10_1 MSAA support
    if (o.msaa)
    {
        appendShaderOption(o.msaa, "USE_MSAA", "1");

        // Number of samples
        {
            c_msaa_samples[0] = char(o.msaa_samples) + '0';
            c_msaa_samples[1] = 0;
            appendShaderOption(o.msaa_samples, "MSAA_SAMPLES", c_msaa_samples);
        }
        // Current sample
        {
            if (m_MSAASample < 0 || o.msaa_opt)
                c_msaa_current_sample[0] = '0';
            else
                c_msaa_current_sample[0] = '0' + char(m_MSAASample);
            c_msaa_current_sample[1] = 0;

            appendShaderOption(m_MSAASample >= 0 ? m_MSAASample : 0,
                "ISAMPLE", c_msaa_current_sample);
        }

        appendShaderOption(o.msaa_opt, "MSAA_OPTIMIZATION", "1");

        switch (o.msaa_alphatest)
        {
        case MSAA_ATEST_DX10_0_ATOC:
            options.add("MSAA_ALPHATEST_DX10_0_ATOC", "1");

            sh_name.append(static_cast<u32>(1)); // DX10_0_ATOC   on
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
            break;
        case MSAA_ATEST_DX10_1_ATOC:
            options.add("MSAA_ALPHATEST_DX10_1_ATOC", "1");

            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(1)); // DX10_1_ATOC   on
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
            break;
        case MSAA_ATEST_DX10_1_NATIVE:
            options.add("MSAA_ALPHATEST_DX10_1", "1");

            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(1)); // DX10_1_NATIVE on
            break;
        default:
            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
        }
    }
    else
    {
        sh_name.append(static_cast<u32>(0)); // MSAA off
        sh_name.append(static_cast<u32>(0)); // No MSAA samples
        sh_name.append(static_cast<u32>(0)); // No MSAA optimization
        sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
        sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
        sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
    }

#ifdef XR_NATIVE
    // D3D12 cannot view a slice past the first of the sun cascade array as a Texture2D, so the
    // shaders read s_smap as an array (shadow.h). Every R5 and R6 shader has it (one array view
    // binds on Vulkan as well): the cache directory below tells the permutations apart.
    options.add("SMAP_ARRAY", "1");
#endif

    // finish
    options.finish();
    sh_name.finish();

    HRESULT _result = E_FAIL;

    char extension[3];
    strncpy_s(extension, pTarget, 2);

    pcstr renderer;
    if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        renderer = "r4" DELIMITER;
    else if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        renderer = "r3" DELIMITER;
    else
    {
        renderer = "r4_level9" DELIMITER;
        R_ASSERT(!"Feature levels lower than 10.0 are unsupported");
    }
#if defined(XR_D3D12)
    renderer = "r5" DELIMITER;
#elif defined(XR_VULKAN)
    renderer = "r6" DELIMITER;
#endif

    string_path filename;
    strconcat(sizeof(filename), filename, renderer, name, ".", extension);

    string_path file_name, pack_name;
    {
        string_path file;
        strconcat(sizeof(file), file, "shaders_cache_grizzly" DELIMITER, filename, DELIMITER, sh_name.c_str());
        strconcat(sizeof(filename), filename, filename, DELIMITER, sh_name.c_str());
        FS.update_path(file_name, "$app_data_root$", file);
        shader_pack_path(pack_name, filename);
    }

    string_path shadersFolder;
    FS.update_path(shadersFolder, "$game_shaders$", RImplementation.getShaderPath());

    u32 fileCrc = 0;
    getFileCrc32(fs, shadersFolder, fileCrc);
    fs->seek(0);

#if defined(XR_VULKAN)
    // Vulkan runs SPIR-V only: every shader is DXC's, there is no FXC to fall back to.
    if (!da_dxc_active())
    {
        Msg("! %s: the Vulkan renderer compiles shaders only through DXC, which is not available", filename);
        return E_FAIL;
    }
    return compile_dxil(name, extension, sh_name.c_str(), fs, fileCrc, pFunctionName, pTarget, options.data(), o.disasm,
        result);
#elif defined(XR_D3D12)
    // DXC for every engine shader but a DXBC twin (RSC_FXC). One DXC cannot build goes to FXC below and
    // its passes get DXBC twins (UnifyShaderFormats); mesh and amplification shaders have only DXC.
    const bool mesh_stage = (pTarget[0] == 'm' || pTarget[0] == 'a') && pTarget[1] == 's';
    if (da_dxc_active() && !(Flags & RSC_FXC))
    {
        _result = compile_dxil(name, extension, sh_name.c_str(), fs, fileCrc, pFunctionName, pTarget, options.data(),
            o.disasm, result);
        if (SUCCEEDED(_result) || mesh_stage)
            return _result;
        da_dxc_note_fxc();
    }
    else if (mesh_stage)
    {
        Msg("! %s: mesh and amplification shaders compile only through DXC", filename);
        return E_FAIL;
    }
#endif
#ifndef XR_VULKAN
    Flags &= ~RSC_FXC;

    // The player's cache first, the release's pack where it has nothing valid.
    for (pcstr cached : {static_cast<pcstr>(file_name), static_cast<pcstr>(pack_name)})
    {
        if (SUCCEEDED(_result) || !cached[0] || !FS.exist(cached))
            continue;
        IReader* file = FS.r_open(cached);
        if (file->length() > 4)
        {
            const bool dx9compatibility = file->r_u32();

            u32 savedFileCrc = file->r_u32();
            if (savedFileCrc == fileCrc)
            {
                u32 savedBytecodeCrc = file->r_u32();
                u32 bytecodeCrc = crc32(file->pointer(), u32(file->elapsed()));
                if (bytecodeCrc == savedBytecodeCrc)
                {
#ifdef DEBUG
                    Log("* Loading shader:", cached);
#endif
                    _result = create_shader(pTarget, (DWORD*)file->pointer(), file->elapsed(), nullptr, 0,
                        filename, result, o.disasm, dx9compatibility, false);
                }
            }
        }
        FS.r_close(file);
    }

    if (FAILED(_result))
    {
        includer Includer;
        LPD3DBLOB pShaderBuf = nullptr;
        LPD3DBLOB pErrorBuf = nullptr;
        u32 compileFlags = Flags;
        if (strstr(Core.Params, "-shader_warnings_as_errors"))
            compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;

        _result = HW.D3DCompile(fs->pointer(), fs->length(), "", options.data(),
            &Includer, pFunctionName, pTarget, compileFlags, 0, &pShaderBuf, &pErrorBuf);

        if (FAILED(_result) && pErrorBuf)
        {
            cpcstr str = static_cast<cpcstr>(pErrorBuf->GetBufferPointer());
            if (strstr(str, "error X3523")) // is there a better way?
            {
                _RELEASE(pShaderBuf);
                _RELEASE(pErrorBuf);
                compileFlags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
                _result = HW.D3DCompile(fs->pointer(), fs->length(), "", options.data(),
                    &Includer, pFunctionName, pTarget, compileFlags, 0, &pShaderBuf, &pErrorBuf);
            }
        }

        if (SUCCEEDED(_result))
        {
            if (pErrorBuf && strstr(Core.Params, "-log_shader_warnings"))
            {
                Log("! Shader compiler warning: ", file_name);
                Log(static_cast<LPCSTR>(pErrorBuf->GetBufferPointer()));
            }

            const bool dx9compatibility = compileFlags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
            IWriter* file = FS.w_open(file_name);

            file->w_u32(dx9compatibility);
            file->w_u32(fileCrc);

            u32 bytecodeCrc = crc32(pShaderBuf->GetBufferPointer(), u32(pShaderBuf->GetBufferSize()));
            file->w_u32(bytecodeCrc); // Do not write anything below this line, take a look at reading (crc32)

            file->w(pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize());
            FS.w_close(file);
#ifdef DEBUG
            Log("- Compile shader:", file_name);
#endif
            _result = create_shader(pTarget, (DWORD*)pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize(),
                nullptr, 0, filename, result, o.disasm, dx9compatibility, false);
        }
        else
        {
            Log("! ", file_name);
            // The file name alone does not identify the failing permutation - dump the macro set,
            // otherwise an option-dependent breakage is unreproducible from the log.
            for (const D3D_SHADER_MACRO* m = options.data(); m && m->Name; ++m)
                Msg("!   with define: %s=%s", m->Name, m->Definition ? m->Definition : "");
            if (pErrorBuf)
                Log("! error: ", (LPCSTR)pErrorBuf->GetBufferPointer());
            else
                Msg("Can't compile shader hr=0x%08x", _result);
        }

        _RELEASE(pShaderBuf);
        _RELEASE(pErrorBuf);
    }

    return _result;
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
