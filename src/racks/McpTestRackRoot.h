#pragma once
#include <juce_core/juce_core.h>

namespace airack::testing {
// Deliberately accept only drive-rooted paths; drive-relative and root-relative paths
// depend on process or drive state and cannot identify an owned test directory.
inline bool fullyQualifiedMcpTestPath(const juce::String& path) {
    if(path.length()<3)return false;
    const auto drive=path[0];
    return ((drive>='A'&&drive<='Z')||(drive>='a'&&drive<='z'))
        && path[1]==':' && (path[2]=='/'||path[2]=='\\');
}
}
