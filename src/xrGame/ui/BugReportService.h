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

bool Submit(pcstr title, pcstr description, pcstr attachmentPath);
State GetState();
xr_string GetMessage();
void Reset();
}
