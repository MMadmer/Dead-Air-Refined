// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Textures that move by themselves. Besides the engine's own Theora video (.ogm), AVI and frame sequence
// (.seq), a texture name can stand for an animated GIF or a video Media Foundation plays (MP4, WebM, WMV):
// the renderer's texture loader asks da_media_resolve which file a name means and, for a GIF or a video,
// uploads the frame a CDaMediaSource hands it. Both loop and are silent.

enum class DaMediaKind
{
    None, // nothing of these: the loader's usual search (.dds in the level, then the game)
    Theora,
    Avi,
    Gif,
    Video,
    Sequence,
    Image,
    Picture, // pixels the game drew itself and published (da_media_publish)
};

// A picture the game draws itself - the menu's community panel: a texture named DA_MEDIA_PICTURE "<id>"
// shows the pixels last published under <id> (RGBA, width * 4 bytes a row, top row first), new ones as
// they come. A texture keeps the size its id had when it was made, so a picture of another size takes
// another id.
#define DA_MEDIA_PICTURE "$da_picture$\\"
ENGINE_API void da_media_publish(pcstr id, u32 width, u32 height, const u8* rgba);
ENGINE_API void da_media_forget(pcstr id);

// The file the texture `name` (relative to textures\) stands for: the first of .ogm, .avi, .gif, .mp4,
// .webm, .wmv, .seq and .dds that exists, except that a loose file - a mod's, in gamedata or an XMS
// module - wins over one in an archive, so a mod replaces the game's video with a picture or a GIF
// of its own under the same name.
ENGINE_API DaMediaKind da_media_resolve(pcstr name, string_path& fn);

// The clock textures move by (ms): real time less the time the application was inactive. Unlike
// Device.dwTimeContinual it also runs while a level loads, between the frames of the loading screen.
ENGINE_API u32 da_media_time();

class ENGINE_API CDaMediaSource
{
public:
    virtual ~CDaMediaSource() = default;

    // A GIF or a video of the file system's path `fn`; nullptr when it cannot be played (said in the
    // log).
    static CDaMediaSource* Open(DaMediaKind kind, pcstr fn);

    u32 Width() const { return m_width; }
    u32 Height() const { return m_height; }

    // Brings the picture to the time `time` (da_media_time); true when Pixels changed. The first call
    // starts the playback.
    virtual bool Update(u32 time) = 0;

    // RGBA, Width() * 4 bytes a row, top row first
    const u8* Pixels() const { return m_pixels; }

protected:
    u32 m_width{};
    u32 m_height{};
    const u8* m_pixels{};
    bool m_started{};
};

#if defined(XR_PLATFORM_WINDOWS)
// A read-only COM stream over a file of the file system, an archive's included: Media Foundation reads a video
// through it. The stream owns the file; the last reference closes it.
struct IStream;
ENGINE_API IStream* da_media_open_stream(pcstr fn);
#endif

// What the interface's own shaders know besides their textures (a menu's or the loading screen's
// background drawn by a mod's shader): the renderer gives these to the constants of the same names of
// any shader that declares them.
struct ENGINE_API DaUIShaderState
{
    // xms_ui_rect, xms_ui_uv: the window being drawn in screen pixels (left, top, right, bottom) and the
    // part of its texture it shows
    Fvector4 rect{};
    Fvector4 uv{};
    // xms_ui_cursor: the cursor in screen pixels
    Fvector2 cursor{};
    // xms_ui_progress: the loading's progress, 0..1, eased and as it is
    float progress_eased{};
    float progress{};
    // xms_ui_time.y counts from here (da_media_time): when the menu or the loading screen opened
    u32 opened{};
};

extern ENGINE_API DaUIShaderState g_da_ui_shader;
