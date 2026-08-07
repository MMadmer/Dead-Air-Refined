////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_simulator_base.h"

#include <span>

class NET_Packet;
struct SaveCaptureQueue;
struct SaveRequestQueue;

class CALifeStorageManager : public virtual CALifeSimulatorBase
{
    friend class CALifeUpdatePredicate;

protected:
    typedef CALifeSimulatorBase inherited;

protected:
    string_path m_save_name;
    LPCSTR m_section;

private:
    void process_save_requests(float budget_milliseconds);
    void process_save_captures(float budget_milliseconds);
    void load(void* buffer, const u32& buffer_size, LPCSTR file_name, u64 save_id,
        bool custom_present, std::span<const u8> custom_data,
        bool custom_backup_present, std::span<const u8> custom_backup_data);
    std::unique_ptr<SaveCaptureQueue> m_save_captures;
    std::unique_ptr<SaveRequestQueue> m_save_requests;

public:
    CALifeStorageManager(IPureServer* server, LPCSTR section);
    virtual ~CALifeStorageManager();
    static bool process_async_save_completions();
    static bool wait_for_pending_saves();
    static bool save_capture_active();
    static bool save_capture_reentrant();
    static bool validate_load_companions(LPCSTR save_name);
    bool load(LPCSTR save_name = 0);
    bool save(LPCSTR save_name = 0, bool update_name = true, bool show_status = true);
    void save(NET_Packet& net_packet);
};

#include "alife_storage_manager_inline.h"
