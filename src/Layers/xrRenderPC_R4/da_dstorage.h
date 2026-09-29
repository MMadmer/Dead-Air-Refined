// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// DirectStorage texture loading on R5 (docs/dead-air/dx12-features/dstorage.md 3.1): the runtime,
// its queues and the texture requests. The R4 module loads every texture the usual way. Both modules
// keep the [texload] numbers of section 3.7.
#pragma once

class CStreamReader;

namespace xray::render::RENDER_NAMESPACE
{
class CTexture;
class CRender;
struct DaDsTicket;

// The runtime is loaded and its queues exist on this device (r__caps dstorage=). Loads it on the first
// call (R5 without -no_dstorage), whether r__dstorage is ticked or not.
bool da_dstorage_available();
// CHW::DestroyDevice, before Streamline goes: drain and release.
void da_dstorage_shutdown();
// r__texload_stats: the DirectStorage state, the last batches and this level's synchronous loads.
void da_dstorage_log_stats();

// ---- DirectStorage textures (R5; R4 never takes one) ------------------------------------------------------
// CTexture::Load of a normal texture: DirectStorage takes it when it can, else null (today's path). On an
// upload batch worker the ticket stays pending until the batch completes: the caller keeps it in
// CTexture::ds_ticket (its only owner) and registers the texture with da_dstorage_batch_add. Elsewhere the
// load is synchronous: the caller ends the ticket at once.
DaDsTicket* da_dstorage_begin(pcstr name);
bool da_dstorage_batch_thread();
void da_dstorage_batch_add(CTexture* texture);
// Waits for the ticket's requests (not after a timeout of the session) and hands over its texture
// (MemoryUsage in `memory`), verified first under -dstorage_verify; null when they failed or did not
// complete - today's path loads it then, counted as failed. Deletes the ticket; requests still in flight
// keep its resources parked until they complete (dstorage.md 8).
ID3DBaseTexture* da_dstorage_end(DaDsTicket* ticket, u32& memory);
// The upload pool's last worker of a batch: submits, waits and finishes every texture of the batch.
void da_dstorage_batch_complete();

// ---- [texload] numbers (dstorage.md 3.7) ----------------------------------------------------------------
// The path a texture took: today's (cpu), DirectStorage raw or LZO, or DirectStorage failed and today's
// path loaded it.
enum class DaTexPath : u8
{
    Cpu,
    DsRaw,
    DsLzo,
    DsFailed,
    Count
};

// One texture_load: its wall time and the CPU spent reading (archive open, LZO inflate), parsing,
// creating the resource and in the codec. `error`: no texture came out.
struct DaTexloadSample
{
    float total_ms{};
    float read_ms{};
    float create_ms{};
    float codec_ms{};
    u64 bytes{};
    DaTexPath path{DaTexPath::Cpu};
    bool error{};
};

// A timestamp for the sample fields (QueryPerformanceCounter ticks) and the milliseconds since one.
u64 da_texload_ticks();
float da_texload_ms_since(u64 ticks);

void da_texload_note(const DaTexloadSample& sample);
// texture_load after a failed DirectStorage load: its sample counts as `failed`.
bool da_texload_take_fallback();

// The texture upload pool (ResourceManager.cpp): every worker thread marks itself once; a batch is begun
// by Start, ended by its last worker and reported (one line) by the first thread that joins it.
void da_texload_batch_worker();
void da_texload_batch_begin(pcstr label, size_t textures, size_t workers);
void da_texload_batch_end();
void da_texload_batch_joined(float wait_ms);

// A level's synchronous loads (outside the pool), split into its load (until the level is ready and its
// precache frames are over) and its play: begin = its final upload (upload_ms = how long that call took),
// end = its unload or the next level load, which prints both groups once.
void da_texload_game_begin(float upload_ms);
void da_texload_game_end();

// ---- level geometry (dstorage.md 3.8, PLAN S9) ------------------------------------------------------------
// CRender::LoadBuffers on R5: the vertex and index buffers of level.geom / level.geomx go from the archive
// straight into their buffers; false = today's path loads them (R4, DirectStorage off or unavailable, an entry
// it cannot take, a failed request).
bool da_dstorage_load_buffers(CRender& render, CStreamReader* fs, bool alternative);
// Times LoadBuffers on either path ([texload] geometry line).
class DaGeometryTimer
{
public:
    explicit DaGeometryTimer(bool alternative);
    ~DaGeometryTimer();
    DaGeometryTimer(const DaGeometryTimer&) = delete;
    DaGeometryTimer& operator=(const DaGeometryTimer&) = delete;

private:
    u64 m_start;
    bool m_alternative;
};

// ---- shared with texture_load (dx11Texture.cpp) ---------------------------------------------------------
// The file texture_load opens for `name` (level, saves, game textures, bump fallbacks); false = none.
// `quiet`: false instead of a logged fallback (default bump map, the "not existing" texture).
bool texture_resolve_name(pcstr name, string_path& fn, bool quiet = false);
// CTexture objects alive now (the leak check of a level change, r__texload_stats).
u32 da_texload_live_textures();
int get_texture_load_lod(LPCSTR fn);
u32 calc_texture_size(int lod, u32 mip_cnt, size_t orig_size);
} // namespace xray::render::RENDER_NAMESPACE
