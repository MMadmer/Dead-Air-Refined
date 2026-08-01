#pragma once

namespace BugReportService
{
enum class State : u8
{
    Idle,
    Sending,
    Succeeded,
    Failed
};

enum class Availability : u8
{
    Checking,
    Available,
    Unavailable
};

bool Submit(pcstr title, pcstr description, pcstr attachmentPath);
State GetState();
xr_string GetMessage();
void Reset();
void CheckAvailability();
Availability GetAvailability();
}
