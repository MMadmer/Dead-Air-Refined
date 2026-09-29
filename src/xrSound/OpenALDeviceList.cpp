/*
 * Copyright (c) 2005, Creative Labs Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided
 * that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice, this list of conditions and
 * 	     the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of conditions
 * 	     and the following disclaimer in the documentation and/or other materials provided with the distribution.
 *     * Neither the name of Creative Labs Inc. nor the names of its contributors may be used to endorse or
 * 	     promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "stdafx.h"

#include "OpenALDeviceList.h"
#include "SoundRender_Core.h"

#include <al.h>
#include <alc.h>

namespace
{
// OpenAL names devices in UTF-8, the menu draws Windows-1251 (every text of the game is in it). The
// menu name keeps what that code page can show - a device named in a script it lacks, such as
// "扬声器 (Realtek(R) Audio)", keeps its Latin part - without the "OpenAL Soft on " every name starts
// with. Opening a device still uses the full UTF-8 name.
xr_string MenuName(pcstr utf8)
{
    constexpr char prefix[] = "OpenAL Soft on ";
    if (!strncmp(utf8, prefix, sizeof(prefix) - 1))
        utf8 += sizeof(prefix) - 1;

    xr_string name;
#if defined(XR_PLATFORM_WINDOWS)
    wchar_t wide[512];
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, int(std::size(wide)));
    for (int i = 0; i + 1 < count; ++i)
    {
        char narrow[4];
        BOOL lossy = FALSE;
        if (WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, &wide[i], 1, narrow, sizeof(narrow), nullptr, &lossy) == 1 &&
            !lossy)
            name += narrow[0];
    }
#else
    name = utf8;
#endif

    // What a dropped script leaves behind: runs of spaces, and brackets around the rest.
    xr_string tidy;
    for (const char c : name)
    {
        if (c != ' ' || (!tidy.empty() && tidy.back() != ' '))
            tidy += c;
    }
    while (!tidy.empty() && tidy.back() == ' ')
        tidy.pop_back();
    if (tidy.size() > 2 && tidy.front() == '(' && tidy.back() == ')')
    {
        int depth = 0;
        size_t closes = 0;
        for (size_t i = 0; i < tidy.size(); ++i)
        {
            depth += tidy[i] == '(' ? 1 : tidy[i] == ')' ? -1 : 0;
            if (!depth)
            {
                closes = i;
                break;
            }
        }
        if (closes + 1 == tidy.size())
            tidy = tidy.substr(1, tidy.size() - 2);
    }
    return tidy;
}
} // namespace

ALDeviceList::ALDeviceList()
{
    snd_device_id = (u32)-1;
    Enumerate();
}

void ALDeviceList::IterateAndAddDevicesString(pcstr devices)
{
    if (!devices)
        return;

    // Opening every endpoint can block for seconds; capabilities are queried only for the selected device.
    while (*devices != '\0')
    {
        m_devices.emplace_back(devices, 1, 1);
        devices += xr_strlen(devices) + 1;
    }
}

void ALDeviceList::Enumerate()
{
#ifndef MASTER_GOLD
    Msg("SOUND: OpenAL: enumerate devices...");
#endif
    // have a set of vectors storing the device list, selection status, spec version #
    // -- empty all the lists and reserve space for 10 devices
    m_devices.clear();
    m_devices.reserve(10);
    m_defaultDeviceName[0] = '\0';

    // grab function pointers for 1.1-API functions, and if successful proceed to enumerate all devices
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT"))
    {
        pcstr devices = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);

        if (pcstr defaultDeviceName = alcGetString(nullptr, ALC_DEFAULT_ALL_DEVICES_SPECIFIER))
            xr_strcpy(m_defaultDeviceName, defaultDeviceName);
        Log("SOUND: OpenAL: system default sound device name is", m_defaultDeviceName);

        IterateAndAddDevicesString(devices);
    }
    // grab function pointers for 1.0-API functions, and if successful proceed to enumerate all devices
    else if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT"))
    {
        pcstr devices = alcGetString(nullptr, ALC_DEVICE_SPECIFIER);

        if (pcstr defaultDeviceName = alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER))
            xr_strcpy(m_defaultDeviceName, defaultDeviceName);
        Log("SOUND: OpenAL: system default sound device name is", m_defaultDeviceName);

        IterateAndAddDevicesString(devices);
    }
    else
    {
        Msg("~ SOUND: OpenAL: EnumerationExtension NOT Present");
    }

#if defined(XR_PLATFORM_WINDOWS)
    constexpr pcstr AL_GENERIC_HARDWARE = "Generic Hardware";
    constexpr pcstr AL_GENERIC_SOFTWARE = "Generic Software";

    if (strstr(Core.Params, "-force_sw_audio") && xr_stricmp(m_defaultDeviceName, AL_GENERIC_HARDWARE) == 0)
    {
        xr_strcpy(m_defaultDeviceName, AL_GENERIC_SOFTWARE);
        Log("SOUND: OpenAL: default sound device name set to", m_defaultDeviceName);
    }
#endif

    // make token
    const auto _cnt = GetNumDevices();

    auto& devices = SoundRender->Parent.GetDevicesList();
    devices.reserve(_cnt + 1);

    for (u32 i = 0; i < _cnt; ++i)
    {
        // The token is what the menu shows and user.ltx keeps, so it has to be unique too.
        xr_string name = MenuName(m_devices[i].name);
        if (name.empty())
            name = "Audio device";
        const xr_string base = name;
        for (u32 n = 2; std::any_of(devices.begin(), devices.end(), [&](const xr_token& t) { return t.name && name == t.name; }); ++n)
            name = base + " (" + std::to_string(n).c_str() + ")";
        devices.emplace_back(xr_strdup(name.c_str()), i);
    }
    devices.emplace_back(nullptr, -1);
    //--

    if (0 == GetNumDevices())
    {
        Log("SOUND: OpenAL: No devices available.");
    }
    else
    {
#ifndef MASTER_GOLD
        Log("SOUND: OpenAL: All available devices:");
        for (u32 j = 0; j < GetNumDevices(); j++)
        {
            Msg("%d. %s %s", j + 1, GetDeviceName(j),
                xr_stricmp(GetDeviceName(j), m_defaultDeviceName) == 0 ? "(default)" : "");
        }
#endif
    }
}

pcstr ALDeviceList::GetDeviceName(size_t index) const
{
    return m_devices[index].name;
}

void ALDeviceList::SelectBestDevice()
{
    if (GetNumDevices() == 0)
    {
        Msg("SOUND: Can't select device. List empty");
        return;
    }

    int best_majorVersion = -1;
    int best_minorVersion = -1;
    int majorVersion;
    int minorVersion;

    if (snd_device_id == (u32)-1)
    {
        // select best
        u32 new_device_id = snd_device_id;
        for (u32 i = 0; i < GetNumDevices(); ++i)
        {
            if (xr_stricmp(m_defaultDeviceName, GetDeviceName(i)) != 0)
                continue;

            GetDeviceVersion(i, &majorVersion, &minorVersion);
            if (majorVersion > best_majorVersion ||
                (majorVersion == best_majorVersion && minorVersion > best_minorVersion))
            {
                best_majorVersion = majorVersion;
                best_minorVersion = minorVersion;
                new_device_id = i;
            }
        }
        if (new_device_id == (u32)-1)
        {
            new_device_id = 0; // first
        }
        snd_device_id = new_device_id;
    }
    Msg("SOUND: Selected device is %s", GetDeviceName(snd_device_id));
}

/*
 * Returns the major and minor version numbers for a device at a specified index in the complete list
 */
void ALDeviceList::GetDeviceVersion(size_t index, int* major, int* minor)
{
    *major = m_devices[index].major_ver;
    *minor = m_devices[index].minor_ver;
}
