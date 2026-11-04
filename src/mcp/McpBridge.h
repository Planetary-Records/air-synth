#pragma once
#include "McpService.h"
namespace airack::mcp
{
struct BridgeOptions { ServiceOptions service; std::optional<juce::Uuid> instance; };
int runStdio(const BridgeOptions&);
}
