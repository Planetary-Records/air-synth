#include "CompanionLaunch.h"
#include <cstdint>
#include <cwchar>

extern "C" __declspec(dllexport) std::uint32_t __cdecl
rackSynthFixtureCompanionPath(wchar_t* output, std::uint32_t capacity)
{
    const auto text=airack::companion::companionExecutable().getFullPathName();
    if(text.isEmpty())return 0;
    const auto* wide=text.toWideCharPointer();
    const auto required=static_cast<std::uint32_t>(std::wcslen(wide)+1);
    if(output && capacity>=required)std::wmemcpy(output,wide,required);
    return required;
}
