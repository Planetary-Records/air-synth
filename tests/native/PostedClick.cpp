#include "PostedClick.h"
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
// JUCE runs its messages in order, so once one posted now has run, everything posted before it has.
bool settle(int timeoutMs)
{
    auto ran=std::make_shared<bool>(false);
    juce::MessageManager::callAsync([ran]{*ran=true;});
    const auto end=juce::Time::getMillisecondCounter()+juce::uint32(timeoutMs);
    while(!*ran&&juce::Time::getMillisecondCounter()<end)juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    return *ran;
}
LRESULT CALLBACK slowMessage(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if(message==WM_APP){Sleep(DWORD(wParam));return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
}

bool clickPosted(juce::Button& button,int timeoutMs)
{
    button.triggerClick();
    return settle(timeoutMs);
}

bool clickRunsBehindSlowMessage(const std::function<void(juce::Button&)>& click)
{
    WNDCLASSW type{};type.lpfnWndProc=slowMessage;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"RackSynthSlowMessage";
    RegisterClassW(&type);
    const auto window=CreateWindowExW(0,type.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,type.hInstance,nullptr);
    if(window==nullptr)return false;
    juce::TextButton button;bool ran=false;button.onClick=[&]{ran=true;};
    // With JUCE's queue empty, the click posts its own window message, behind the slow one.
    bool result=settle(5000)&&PostMessageW(window,WM_APP,150,0);
    if(result){click(button);result=ran;}
    settle(5000);DestroyWindow(window);
    return result;
}
