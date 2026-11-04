cmake_minimum_required(VERSION 3.22)
project(RackSynthFeasibility VERSION 0.1.0 LANGUAGES C CXX)
# What a musician is told. The numeric project version above stays put so hosts keep recognising the plug-in.
set(RACKSYNTH_RELEASE_VERSION "0.1.0-alpha.2")
# Shipping builds carry their C++ runtime. Probe builds retain the frozen numerical compiler policy; Debug needs its DLL hooks.
option(RACKSYNTH_BUILD_PROBES "Build development feasibility probes (requires Python)" ON)
if(NOT RACKSYNTH_BUILD_PROBES)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG be29c81492b6151c8ea8d14c840e1311963b3a83)
FetchContent_MakeAvailable(JUCE)
if(WIN32)
    include(tools/JuceGraphics.cmake)
    set(racksynth_graphics_modules "${CMAKE_BINARY_DIR}/racksynth-juce/modules")
    racksynth_prepare_juce_graphics("${juce_SOURCE_DIR}/modules" "${racksynth_graphics_modules}")
    foreach(property INTERFACE_SOURCES INTERFACE_JUCE_MODULE_SOURCES INTERFACE_JUCE_MODULE_HEADERS INTERFACE_JUCE_MODULE_PATH)
        get_target_property(sources juce_graphics ${property})
        string(REPLACE "${juce_SOURCE_DIR}/modules" "${racksynth_graphics_modules}" sources "${sources}")
        set_property(TARGET juce_graphics PROPERTY ${property} "${sources}")
    endforeach()
endif()
enable_testing()
include(tools/NativeTests.cmake)
option(RACKSYNTH_PARALLEL_COMPILE "Compile two files per MSVC project (CI builds at most eight projects)" OFF)
if(MSVC AND RACKSYNTH_PARALLEL_COMPILE)
    add_compile_options("$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:RACKSYNTH_SATURATION_FROZEN_COMPILER_POLICY>>>>:/MP2>")
endif()
add_subdirectory(src/dsp/space)
if(RACKSYNTH_BUILD_PROBES)
    add_subdirectory(probes/space-reverb)
endif()
# JUCE's CMake API asks for modules to be linked PRIVATE: linked PUBLIC, their sources pass on and every target
# downstream compiles them again. A target that builds modules passes on only what compiling against them needs.
function(racksynth_juce_modules target)
    target_link_libraries(${target} PRIVATE ${ARGN})
    target_include_directories(${target} INTERFACE $<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} INTERFACE $<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>)
    target_compile_options(${target} INTERFACE "$<FILTER:$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>,EXCLUDE,^/MP2$>")
endfunction()
# The modules every library uses, built once; on Windows they compile the same under every definition this build sets.
add_library(RackSynthJuce STATIC)
target_compile_definitions(RackSynthJuce PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NOMINMAX)
racksynth_juce_modules(RackSynthJuce juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_cryptography juce::juce_dsp)
# Insert after RackSynthJuce is defined, outside RACKSYNTH_BUILD_PROBES.
if(WIN32)
    enable_language(ASM_MASM)
    add_library(RackSynthSaturationPowerRuntime STATIC src/dsp/SaturationGainImport.asm)
endif()
add_library(RackSynthSaturation STATIC src/dsp/Saturation.cpp src/dsp/SaturationWideNetwork.cpp)
target_include_directories(RackSynthSaturation PUBLIC src/dsp)
target_link_libraries(RackSynthSaturation PUBLIC RackSynthJuce)
if(WIN32)
    target_link_libraries(RackSynthSaturation PRIVATE RackSynthSaturationPowerRuntime)
endif()
# Carry over any source-specific settings without choosing new ISA/FP settings.
foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FLAGS)
    get_source_file_property(saturation_wide_property probes/saturation/SaturationWideNetwork.cpp ${property})
    if(NOT saturation_wide_property STREQUAL "NOTFOUND")
        set_source_files_properties(src/dsp/SaturationWideNetwork.cpp PROPERTIES ${property} "${saturation_wide_property}")
    endif()
endforeach()
juce_add_console_app(RackSynthSaturationTests PRODUCT_NAME "RackSynthSaturationTests")
target_sources(RackSynthSaturationTests PRIVATE tests/native/SaturationTests.cpp)
target_compile_definitions(RackSynthSaturationTests PRIVATE
    RACKSYNTH_LIFECYCLE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261023-stable-v2"
    RACKSYNTH_LIFECYCLE_RESULT_DIR="${CMAKE_BINARY_DIR}/saturation-release-mvp/$<CONFIG>")
target_link_libraries(RackSynthSaturationTests PRIVATE RackSynthSaturation)
foreach(mode admission functional audition fixtures tail)
    racksynth_add_test(NAME RackSynthSaturationTests_${mode} COMMAND RackSynthSaturationTests --${mode})
endforeach()


add_library(RackSynthDrumAssets STATIC
    src/drums/DrumAsset.cpp src/drums/DrumDocument.cpp src/drums/DrumBundle.cpp)
target_include_directories(RackSynthDrumAssets PUBLIC src/drums)
target_link_libraries(RackSynthDrumAssets PUBLIC RackSynthJuce)
add_executable(RackSynthDrumAssetTests tests/native/DrumAssetTests.cpp)
target_link_libraries(RackSynthDrumAssetTests PRIVATE RackSynthDrumAssets)
racksynth_add_test(NAME RackSynthDrumAssetTests COMMAND RackSynthDrumAssetTests)
add_library(RackSynthDrumEngine STATIC src/drums/DrumEngine.cpp)
target_include_directories(RackSynthDrumEngine PUBLIC src/drums)
target_link_libraries(RackSynthDrumEngine PUBLIC RackSynthDrumAssets)
add_executable(RackSynthDrumEngineTests tests/native/DrumEngineTests.cpp)
target_link_libraries(RackSynthDrumEngineTests PRIVATE RackSynthDrumEngine)
racksynth_add_test(NAME RackSynthDrumEngineTests COMMAND RackSynthDrumEngineTests)
add_executable(RackSynthDrumRoutedTests tests/native/DrumRoutedTests.cpp)
target_link_libraries(RackSynthDrumRoutedTests PRIVATE RackSynthState RackSynthRouted RackSynthDrumEngine)
racksynth_add_test(NAME RackSynthDrumRoutedTests COMMAND RackSynthDrumRoutedTests)
add_library(RackSynthDrumPreparation STATIC src/drums/DrumPreparation.cpp)
target_include_directories(RackSynthDrumPreparation PUBLIC src/drums)
target_link_libraries(RackSynthDrumPreparation PUBLIC RackSynthDrumAssets)
add_executable(RackSynthDrumPreparationTests tests/native/DrumPreparationTests.cpp
    src/drums/DrumPreparation.cpp)
target_compile_definitions(RackSynthDrumPreparationTests PRIVATE AIRACK_DRUM_PREPARATION_TESTS=1)
target_link_libraries(RackSynthDrumPreparationTests PRIVATE RackSynthDrumAssets)
racksynth_add_test(NAME RackSynthDrumPreparationTests COMMAND RackSynthDrumPreparationTests
    "${CMAKE_BINARY_DIR}/drum-preparation-tests")
add_executable(RackSynthDrumBundleTests tests/native/DrumBundleTests.cpp)
target_link_libraries(RackSynthDrumBundleTests PRIVATE RackSynthDrumAssets)
racksynth_add_test(NAME RackSynthDrumBundleTests COMMAND RackSynthDrumBundleTests)
add_library(RackSynthJson STATIC src/ai/StrictJson.cpp)
target_include_directories(RackSynthJson PUBLIC src/ai)
target_link_libraries(RackSynthJson PUBLIC RackSynthJuce)

add_library(RackSynthMcpProtocol STATIC src/mcp/McpProtocol.cpp)
target_include_directories(RackSynthMcpProtocol PUBLIC src/mcp)
target_link_libraries(RackSynthMcpProtocol PUBLIC RackSynthJson)
if(WIN32)
    target_link_libraries(RackSynthMcpProtocol PRIVATE bcrypt)
endif()
juce_add_console_app(RackSynthMcpProtocolTests PRODUCT_NAME "RackSynthMcpProtocolTests")
target_sources(RackSynthMcpProtocolTests PRIVATE tests/native/McpProtocolTests.cpp)
target_link_libraries(RackSynthMcpProtocolTests PRIVATE RackSynthMcpProtocol)
if(WIN32)
    racksynth_add_test(NAME RackSynthMcpProtocolTests COMMAND RackSynthMcpProtocolTests $<TARGET_FILE:RackSynthCompanion>)
else()
    racksynth_add_test(NAME RackSynthMcpProtocolTests COMMAND RackSynthMcpProtocolTests)
endif()
if(WIN32)
    add_library(RackSynthGenerationLog STATIC src/diagnostics/GenerationLog.cpp)
    target_include_directories(RackSynthGenerationLog PUBLIC src/diagnostics)
    target_link_libraries(RackSynthGenerationLog PUBLIC RackSynthJuce PRIVATE shell32 ole32)
    add_executable(RackSynthGenerationLogTests tests/native/GenerationLogTests.cpp)
    target_link_libraries(RackSynthGenerationLogTests PRIVATE RackSynthGenerationLog)
    racksynth_add_test(NAME RackSynthGenerationLogTests COMMAND RackSynthGenerationLogTests "${CMAKE_BINARY_DIR}/generation-diagnostic-tests")
    add_library(RackSynthOAuthCore STATIC probes/oauth/OAuthProbe.cpp probes/oauth/OAuthWindows.cpp)
    target_include_directories(RackSynthOAuthCore PUBLIC probes/oauth)
    target_link_libraries(RackSynthOAuthCore PUBLIC RackSynthJuce RackSynthJson RackSynthGenerationLog PRIVATE bcrypt crypt32 winhttp advapi32)
endif()
if(RACKSYNTH_BUILD_PROBES)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/apply_juce_probe_patch.py"
    --juce-root "${juce_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)

juce_add_plugin(RackSynthProbe
    COMPANY_NAME "AI Rack Synth Feasibility"
    BUNDLE_ID "local.airacksynth.probe"
    PLUGIN_MANUFACTURER_CODE Arsp
    PLUGIN_CODE Mzpr
    FORMATS VST3
    PRODUCT_NAME "Rack Synth Probe"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(RackSynthProbe PRIVATE probes/native/ProbeProcessor.cpp probes/native/ProbeHostDiagnostics.cpp probes/native/ProbeEditor.cpp)
target_include_directories(RackSynthProbe PUBLIC probes/native
    "${juce_SOURCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK")
target_compile_definitions(RackSynthProbe PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_definitions(RackSynthProbe_VST3 PRIVATE PROBE_HOST_DIAGNOSTICS=1)
target_link_libraries(RackSynthProbe PRIVATE
    juce::juce_audio_utils juce::juce_cryptography)
# juce_add_plugin passes the probe's definitions on; its tests also need the modules' include paths and options.
target_include_directories(RackSynthProbe INTERFACE $<TARGET_PROPERTY:RackSynthProbe,INCLUDE_DIRECTORIES>)
target_compile_options(RackSynthProbe INTERFACE $<TARGET_PROPERTY:RackSynthProbe,COMPILE_OPTIONS>)

juce_add_console_app(RackSynthProbeTests PRODUCT_NAME "RackSynthProbeTests")
target_sources(RackSynthProbeTests PRIVATE probes/native/ProbeTests.cpp)
target_link_libraries(RackSynthProbeTests PRIVATE RackSynthProbe)
racksynth_add_test(NAME RackSynthProbeTests COMMAND RackSynthProbeTests)
racksynth_add_test(NAME RackSynthJucePatchTests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/apply_juce_probe_patch.py" --self-test)

if(WIN32)
    juce_add_console_app(RackSynthOAuthProbe PRODUCT_NAME "RackSynthOAuthProbe")
    target_sources(RackSynthOAuthProbe PRIVATE probes/oauth/OAuthProbeMain.cpp)
    target_link_libraries(RackSynthOAuthProbe PRIVATE RackSynthOAuthCore shell32 ole32)
    juce_add_console_app(RackSynthOAuthProbeTests PRODUCT_NAME "RackSynthOAuthProbeTests")
    target_sources(RackSynthOAuthProbeTests PRIVATE probes/oauth/OAuthProbeTests.cpp)
    target_link_libraries(RackSynthOAuthProbeTests PRIVATE RackSynthOAuthCore RackSynthAiProposal bcrypt)
    racksynth_add_test(NAME RackSynthOAuthProbeTests COMMAND RackSynthOAuthProbeTests)
endif()
endif()

add_library(RackSynthCore STATIC src/dsp/SynthEngine.cpp)
target_include_directories(RackSynthCore PUBLIC src/dsp)
target_link_libraries(RackSynthCore PUBLIC RackSynthJuce)
juce_add_console_app(RackSynthCoreTests PRODUCT_NAME "RackSynthCoreTests")
target_sources(RackSynthCoreTests PRIVATE tests/native/SynthEngineTests.cpp)
target_link_libraries(RackSynthCoreTests PRIVATE RackSynthCore)
racksynth_add_test(NAME RackSynthCoreTests COMMAND RackSynthCoreTests)
# What a musician can send us and the marker that notices a session that did not end normally (pure logic, no network).
add_library(RackSynthFeedback STATIC src/diagnostics/FeedbackReport.cpp src/diagnostics/SessionMarker.cpp)
target_include_directories(RackSynthFeedback PUBLIC src/diagnostics)
target_link_libraries(RackSynthFeedback PUBLIC RackSynthJuce)
juce_add_console_app(RackSynthFeedbackTests PRODUCT_NAME "RackSynthFeedbackTests")
target_sources(RackSynthFeedbackTests PRIVATE tests/native/FeedbackReportTests.cpp)
target_link_libraries(RackSynthFeedbackTests PRIVATE RackSynthFeedback)
racksynth_add_test(NAME RackSynthFeedbackTests COMMAND RackSynthFeedbackTests)

add_library(RackSynthState STATIC src/state/ParameterRegistry.cpp src/state/SoundState.cpp src/state/SoundDocument.cpp src/state/FactorySounds.cpp src/state/DynamicEqEdits.cpp src/state/EqCurveFit.cpp src/studio/EqComparison.cpp src/racks/RackDesign.cpp src/racks/RackReconciliation.cpp src/racks/RackLibrary.cpp src/racks/RackExamples.cpp src/racks/UserReach.cpp)
target_include_directories(RackSynthState PUBLIC src/state src/studio)
target_link_libraries(RackSynthState PUBLIC RackSynthRouted RackSynthJson RackSynthJuce RackSynthSpaceReverbControls)
juce_add_console_app(RackSynthRackPackageExport PRODUCT_NAME "RackSynthRackPackageExport")
target_sources(RackSynthRackPackageExport PRIVATE tools/native/ExportRackPackages.cpp)
target_link_libraries(RackSynthRackPackageExport PRIVATE RackSynthState RackSynthJuceInstrumentApps RackSynthInstrument)
target_compile_definitions(RackSynthRackPackageExport PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
set_target_properties(RackSynthRackPackageExport PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_library(RackSynthRackPackage STATIC src/racks/RackPackage.cpp)
target_link_libraries(RackSynthRackPackage PUBLIC RackSynthState)
juce_add_console_app(RackSynthRackPackageTests PRODUCT_NAME "RackSynthRackPackageTests")
target_sources(RackSynthRackPackageTests PRIVATE tests/native/RackPackageTests.cpp tests/native/RackPackageFixtures.cpp)
target_compile_definitions(RackSynthRackPackageTests PRIVATE
    RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1"
    RACKSYNTH_PACKAGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthRackPackageTests PRIVATE RackSynthRackPackage)
racksynth_add_test(NAME RackSynthRackPackageTests COMMAND RackSynthRackPackageTests)
add_library(RackSynthHouseSkinModel STATIC src/studio/HouseSkin.cpp)
target_link_libraries(RackSynthHouseSkinModel PUBLIC RackSynthState PRIVATE RackSynthHouseSkin)
add_library(RackSynthRackPackageStore STATIC src/racks/RackPackageStore.cpp)
target_link_libraries(RackSynthRackPackageStore PUBLIC RackSynthRackPackage RackSynthHouseSkinModel PRIVATE RackSynthStudioSkin)
if(WIN32)
    target_link_libraries(RackSynthRackPackageStore PRIVATE shell32 ole32)
endif()
juce_add_console_app(RackSynthRackPackageStoreTests PRODUCT_NAME "RackSynthRackPackageStoreTests")
target_sources(RackSynthRackPackageStoreTests PRIVATE tests/native/RackPackageStoreTests.cpp tests/native/RackPackageFixtures.cpp)
target_sources(RackSynthRackPackageStoreTests PRIVATE src/racks/RackPackageStore.cpp)
target_compile_definitions(RackSynthRackPackageStoreTests PRIVATE AIRACK_STORE_TESTS=1)
target_compile_definitions(RackSynthRackPackageStoreTests PRIVATE
    RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1")
target_link_libraries(RackSynthRackPackageStoreTests PRIVATE RackSynthRackPackageStore)
racksynth_juce_modules(RackSynthRackPackageStoreTests juce::juce_gui_basics)
racksynth_add_test(NAME RackSynthRackPackageStoreTests COMMAND RackSynthRackPackageStoreTests "${CMAKE_BINARY_DIR}/../rack-library-native/task2/trials")
add_library(RackSynthRackInventory STATIC src/racks/RackInventory.cpp)
add_library(RackSynthJuceGraphics STATIC)
target_compile_definitions(RackSynthJuceGraphics PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NOMINMAX)
racksynth_juce_modules(RackSynthJuceGraphics juce::juce_graphics)
target_include_directories(RackSynthRackInventory PRIVATE src/pattern)
target_link_libraries(RackSynthRackInventory PUBLIC RackSynthRackPackageStore RackSynthJuceGraphics)
add_library(RackSynthRackLibraryClient STATIC src/racks/RackLibraryClient.cpp src/racks/RackLibraryHttpWindows.cpp)
target_link_libraries(RackSynthRackLibraryClient PUBLIC RackSynthRackInventory PRIVATE winhttp)
juce_add_console_app(RackSynthRackLibraryClientTests PRODUCT_NAME "RackSynthRackLibraryClientTests")
target_sources(RackSynthRackLibraryClientTests PRIVATE tests/native/RackLibraryClientTests.cpp tests/native/RackPackageFixtures.cpp)
target_sources(RackSynthRackLibraryClientTests PRIVATE src/racks/RackLibraryClient.cpp)
target_compile_definitions(RackSynthRackLibraryClientTests PRIVATE AIRACK_LIBRARY_CACHE_TESTS=1)
target_compile_definitions(RackSynthRackLibraryClientTests PRIVATE RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1")
target_compile_definitions(RackSynthRackLibraryClientTests PRIVATE RACKSYNTH_CLIENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library-client")
target_link_libraries(RackSynthRackLibraryClientTests PRIVATE RackSynthRackLibraryClient)
racksynth_juce_modules(RackSynthRackLibraryClientTests juce::juce_gui_basics)
racksynth_add_test(NAME RackSynthRackLibraryClientTests COMMAND RackSynthRackLibraryClientTests "${CMAKE_BINARY_DIR}/../rack-library-native/task5/trials")
juce_add_console_app(RackSynthRackLibraryHttpWindowsTests PRODUCT_NAME "RackSynthRackLibraryHttpWindowsTests")
target_sources(RackSynthRackLibraryHttpWindowsTests PRIVATE tests/native/RackLibraryHttpWindowsTests.cpp)
target_sources(RackSynthRackLibraryHttpWindowsTests PRIVATE src/racks/RackLibraryHttpWindows.cpp)
target_compile_definitions(RackSynthRackLibraryHttpWindowsTests PRIVATE AIRACK_LIBRARY_HTTP_TESTS=1)
target_link_libraries(RackSynthRackLibraryHttpWindowsTests PRIVATE RackSynthRackLibraryClient)
target_link_libraries(RackSynthRackLibraryHttpWindowsTests PRIVATE ws2_32)
racksynth_juce_modules(RackSynthRackLibraryHttpWindowsTests juce::juce_gui_basics)
racksynth_add_test(NAME RackSynthRackLibraryHttpWindowsTests COMMAND RackSynthRackLibraryHttpWindowsTests "${CMAKE_BINARY_DIR}/../rack-library-native/task5/http-trials")
add_library(RackSynthRackLibraryHttpOwner SHARED tests/native/RackLibraryHttpOwner.cpp src/racks/RackLibraryClient.cpp src/racks/RackLibraryHttpWindows.cpp)
target_compile_definitions(RackSynthRackLibraryHttpOwner PRIVATE AIRACK_LIBRARY_HTTP_TESTS=1)
target_link_libraries(RackSynthRackLibraryHttpOwner PRIVATE RackSynthRackInventory RackSynthNativePreviews winhttp)
racksynth_juce_modules(RackSynthRackLibraryHttpOwner juce::juce_gui_basics)
target_compile_definitions(RackSynthRackLibraryHttpWindowsTests PRIVATE RACKSYNTH_LIBRARY_HTTP_OWNER_DLL=L\"$<TARGET_FILE:RackSynthRackLibraryHttpOwner>\")
add_dependencies(RackSynthRackLibraryHttpWindowsTests RackSynthRackLibraryHttpOwner)
juce_add_console_app(RackSynthRackInventoryTests PRODUCT_NAME "RackSynthRackInventoryTests")
target_sources(RackSynthRackInventoryTests PRIVATE tests/native/RackInventoryTests.cpp tests/native/RackPackageFixtures.cpp)
target_include_directories(RackSynthRackInventoryTests PRIVATE src/pattern)
target_compile_definitions(RackSynthRackInventoryTests PRIVATE
    RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1")
target_link_libraries(RackSynthRackInventoryTests PRIVATE RackSynthRackInventory RackSynthAiProposal)
racksynth_add_test(NAME RackSynthRackInventoryTests COMMAND RackSynthRackInventoryTests "${CMAKE_BINARY_DIR}/../rack-library-native/task3/trials")
racksynth_add_test(NAME RackSynthRackInventoryBoundariesTests COMMAND RackSynthRackInventoryTests "${CMAKE_BINARY_DIR}/../rack-library-native/task3/boundary-trials" --boundaries)
racksynth_add_test(NAME RackSynthRackInventoryCanonicalOverflowTests COMMAND RackSynthRackInventoryTests "${CMAKE_BINARY_DIR}/../rack-library-native/task3/canonical-trials" --canonical-overflow)
juce_add_console_app(RackSynthRackDesignTests PRODUCT_NAME "RackSynthRackDesignTests")
target_sources(RackSynthRackDesignTests PRIVATE tests/native/RackDesignTests.cpp)
target_link_libraries(RackSynthRackDesignTests PRIVATE RackSynthState)
racksynth_add_test(NAME RackSynthRackDesignTests COMMAND RackSynthRackDesignTests)
juce_add_console_app(RackSynthRackReconciliationTests PRODUCT_NAME "RackSynthRackReconciliationTests")
target_sources(RackSynthRackReconciliationTests PRIVATE tests/native/RackReconciliationTests.cpp)
target_link_libraries(RackSynthRackReconciliationTests PRIVATE RackSynthState)
racksynth_add_test(NAME RackSynthRackReconciliationTests COMMAND RackSynthRackReconciliationTests)
juce_add_console_app(RackSynthStateTests PRODUCT_NAME "RackSynthStateTests")
target_sources(RackSynthStateTests PRIVATE tests/native/SoundStateTests.cpp)
target_compile_definitions(RackSynthStateTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
target_link_libraries(RackSynthStateTests PRIVATE RackSynthState)
racksynth_add_test(NAME RackSynthStateTests COMMAND RackSynthStateTests)

juce_add_plugin(RackSynthInstrument
    COMPANY_NAME "AI Rack Synth"
    BUNDLE_ID "local.airacksynth.instrument"
    PLUGIN_MANUFACTURER_CODE Arsp
    PLUGIN_CODE Ars1
    FORMATS VST3 Standalone
    # Preserve artifact paths and IDs; air is the future display name.
    PRODUCT_NAME "AI Rack Synth"
    PLUGIN_NAME "air"
    ICON_BIG resources/branding/ai-rack-icon.svg
    ICON_SMALL resources/branding/ai-rack-icon.svg
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentProcessor.cpp src/plugin/InstrumentEditor.cpp src/plugin/FeedbackPanel.cpp src/plugin/PluginEntry.cpp src/plugin/RackPanel.cpp src/plugin/RackReview.cpp src/plugin/RackOrder.cpp src/plugin/RackStack.cpp)
target_include_directories(RackSynthInstrument PUBLIC src/plugin src/studio)
target_compile_definitions(RackSynthInstrument PUBLIC
    RACKSYNTH_RELEASE_VERSION="${RACKSYNTH_RELEASE_VERSION}"
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(RackSynthInstrument PUBLIC RackSynthState RackSynthFeedback PRIVATE juce::juce_audio_utils juce::juce_cryptography juce::juce_dsp)
juce_add_binary_data(RackSynthNativePreviews HEADER_NAME NativePreviewData.h NAMESPACE NativePreviewData SOURCES
    resources/studio-skin/library-previews/provenance.json
    resources/studio-skin/library-previews/9138771513d34c57a47912251842a402.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842a802.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842aa02.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842b402.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842b802.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842bb01.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842bc01.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842bd02.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842be01.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842c401.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842c802.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842d402.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842d901.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842e402.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842e902.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842f402.png
    resources/studio-skin/library-previews/9138771513d34c57a47912251842f901.png)
target_link_libraries(RackSynthRackLibraryClient PUBLIC RackSynthNativePreviews)

# The approved studio window skin, embedded byte for byte; resources/studio-skin/SOURCE.json records where it came from.
juce_add_binary_data(RackSynthStudioSkin HEADER_NAME StudioSkinData.h NAMESPACE StudioSkinData SOURCES
    resources/studio-skin/elements.json
    resources/studio-skin/fonts/IBMPlexSansCondensed-OFL.txt
    resources/studio-skin/fonts/IBMPlexSansCondensed-Regular.ttf
    resources/studio-skin/fonts/Inconsolata-Regular.ttf
    resources/studio-skin/fonts/LICENSE.txt
    resources/studio-skin/fonts/PermanentMarker-LICENSE.txt
    resources/studio-skin/fonts/PermanentMarker-Regular.ttf
    resources/studio-skin/parts/ai-account-amber.png
    resources/studio-skin/parts/ai-account-green.png
    resources/studio-skin/parts/ai-linked-lit.png
    resources/studio-skin/parts/ai-mode-lit.png
    resources/studio-skin/parts/pattern-bars-pointer.png
    resources/studio-skin/parts/pattern-channel-pointer.png
    resources/studio-skin/parts/pattern-grid-pointer.png
    resources/studio-skin/parts/pattern-key-lit.png
    resources/studio-skin/parts/pattern-length-pointer.png
    resources/studio-skin/parts/pattern-meter-pointer.png
    resources/studio-skin/parts/pattern-mode-down.png
    resources/studio-skin/parts/pattern-mode-up.png
    resources/studio-skin/parts/pattern-pitch-pointer.png
    resources/studio-skin/parts/pattern-playing-lit.png
    resources/studio-skin/parts/pattern-start-pointer.png
    resources/studio-skin/parts/pattern-swing-pointer.png
    resources/studio-skin/parts/pattern-velocity-pointer.png
    resources/studio-skin/parts/programmer-button.png
    resources/studio-skin/parts/programmer-knob-body.png
    resources/studio-skin/parts/programmer-knob-pointer.png
    resources/studio-skin/parts/programmer-led-lit.png
    resources/studio-skin/parts/programmer-led.png
    resources/studio-skin/parts/programmer-lock.png
    resources/studio-skin/parts/programmer-tape.png
    resources/studio-skin/parts/session-ai-lit.png
    resources/studio-skin/parts/session-display-amber.png
    resources/studio-skin/parts/session-error-lit.png
    resources/studio-skin/parts/session-rack-down.png
    resources/studio-skin/parts/session-rack-up.png
    resources/studio-skin/parts/strip-display-amber.png
    resources/studio-skin/parts/strip-mode-down.png
    resources/studio-skin/parts/strip-mode-up.png
    resources/studio-skin/plates/advanced.png
    resources/studio-skin/plates/ai.png
    resources/studio-skin/plates/library.png
    resources/studio-skin/plates/pattern.png
    resources/studio-skin/plates/session.png
    resources/studio-skin/plates/strip.png)
target_link_libraries(RackSynthRackPackageStoreTests PRIVATE RackSynthStudioSkin)
# The approved AI Rack Synth unit skin, embedded byte for byte; resources/house-skin/SOURCE.json records where it came
# from. Binary data keeps file names only, and both skins have plates/strip.png, so it has its own target and namespace.
juce_add_binary_data(RackSynthHouseSkin HEADER_NAME HouseSkinData.h NAMESPACE HouseSkinData SOURCES
    resources/branding/ai-rack-badge.png
    resources/branding/ai-rack-wordmark.svg
    resources/house-skin/elements.json
    resources/house-skin/parts/strip-led-lit.png
    resources/house-skin/parts/strip-macro-1-pointer.png
    resources/house-skin/parts/strip-macro-2-pointer.png
    resources/house-skin/parts/strip-macro-3-pointer.png
    resources/house-skin/parts/strip-macro-4-pointer.png
    resources/house-skin/parts/strip-macro-5-pointer.png
    resources/house-skin/parts/strip-macro-6-pointer.png
    resources/house-skin/parts/strip-macro-7-pointer.png
    resources/house-skin/parts/strip-macro-8-pointer.png
    resources/house-skin/parts/strip-master-pointer.png
    resources/house-skin/parts/unit-fader-cap.png
    resources/house-skin/parts/unit-gain-pointer.png
    resources/house-skin/parts/unit-glide-pointer.png
    resources/house-skin/parts/unit-led-lit.png
    resources/house-skin/parts/unit-macro-1-pointer.png
    resources/house-skin/parts/unit-macro-2-pointer.png
    resources/house-skin/parts/unit-macro-3-pointer.png
    resources/house-skin/parts/unit-macro-4-pointer.png
    resources/house-skin/parts/unit-macro-5-pointer.png
    resources/house-skin/parts/unit-macro-6-pointer.png
    resources/house-skin/parts/unit-macro-7-pointer.png
    resources/house-skin/parts/unit-macro-8-pointer.png
    resources/house-skin/parts/unit-master-pointer.png
    resources/house-skin/parts/unit-pan-1-pointer.png
    resources/house-skin/parts/unit-pan-2-pointer.png
    resources/house-skin/parts/unit-pan-3-pointer.png
    resources/house-skin/parts/unit-pan-4-pointer.png
    resources/house-skin/parts/unit-tape.png
    resources/house-skin/plates/strip.png
    resources/house-skin/plates/unit.png)
target_sources(RackSynthInstrument PRIVATE
    src/studio/StudioSkin.cpp
    src/studio/DotMatrix.cpp
    src/studio/SkinControls.cpp
    src/studio/ScreenControls.cpp
    src/studio/SkinView.cpp
    src/studio/PatternControls.cpp
    src/studio/ProgrammerLook.cpp
    src/studio/ProgrammerValues.cpp
    src/studio/StudioServices.cpp
    src/studio/MidiDrag.cpp
    src/studio/SessionUnit.cpp
    src/studio/AiUnit.cpp
    src/studio/LibraryUnit.cpp
    src/studio/PatternStrip.cpp
    src/studio/PatternUnit.cpp
    src/studio/MakerFace.cpp
    src/studio/Motion.cpp
    src/studio/ParameterGesture.cpp
    src/studio/HouseControls.cpp
    src/studio/HouseUnit.cpp
    src/studio/ProgrammerUnit.cpp
    src/studio/AllControlsPanel.cpp
    src/studio/EmptyRackSpace.cpp)
target_link_libraries(RackSynthInstrument PRIVATE RackSynthStudioSkin RackSynthHouseSkin RackSynthHouseSkinModel)
target_link_libraries(RackSynthInstrument PUBLIC RackSynthRackInventory RackSynthRackLibraryClient)
if(WIN32)
    target_link_libraries(RackSynthInstrument PRIVATE gdiplus)
endif()
# The instrument's modules, built once for the console apps that run it in process; their checks pump the message loop,
# which needs modal loops. It takes the plugin's definitions without linking the plugin, so it builds alongside it.
add_library(RackSynthJuceInstrumentApps OBJECT)
target_compile_definitions(RackSynthJuceInstrumentApps PRIVATE JUCE_STANDALONE_APPLICATION=1 _CONSOLE=1 JUCE_MODAL_LOOPS_PERMITTED=1
    $<TARGET_PROPERTY:RackSynthInstrument,INTERFACE_COMPILE_DEFINITIONS>)
racksynth_juce_modules(RackSynthJuceInstrumentApps juce::juce_audio_utils juce::juce_cryptography juce::juce_dsp)
juce_add_console_app(RackSynthInstrumentTests PRODUCT_NAME "RackSynthInstrumentTests")
target_sources(RackSynthInstrumentTests PRIVATE tests/native/InstrumentTests.cpp tests/native/PostedClick.cpp)
target_link_libraries(RackSynthInstrumentTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthInstrumentTests COMMAND RackSynthInstrumentTests)
racksynth_add_test(NAME RackSynthInstrumentTests_saturation_release COMMAND RackSynthInstrumentTests --saturation-release)
# Its complete editor check opens the preset chooser, a window on the desktop, so it runs apart under the desktop lock.
racksynth_add_test(NAME RackSynthInstrumentTests_complete_editor COMMAND RackSynthInstrumentTests --complete-editor-check)
set_tests_properties(RackSynthInstrumentTests PROPERTIES ENVIRONMENT "AIRACK_INSTRUMENT_EDITOR_APART=1")

juce_add_console_app(RackSynthRender PRODUCT_NAME "RackSynthRender")
target_sources(RackSynthRender PRIVATE tools/native/RenderFixtures.cpp)
target_compile_definitions(RackSynthRender PRIVATE JUCE_PLUGINHOST_VST3=1
    RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}" RACKSYNTH_CONFIGURATION="$<CONFIG>")
# Hosting VST3 is a configuration of its own, so the renderer builds its modules itself.
target_link_libraries(RackSynthRender PRIVATE RackSynthProject RackSynthInstrument juce::juce_audio_utils juce::juce_cryptography juce::juce_dsp)
racksynth_add_test(NAME RackSynthRenderSelfTest COMMAND RackSynthRender --self-test)
juce_add_console_app(RackSynthRackLibraryProductTests PRODUCT_NAME "RackSynthRackLibraryProductTests")
target_sources(RackSynthRackLibraryProductTests PRIVATE tests/native/RackLibraryProductTests.cpp tests/native/RackPackageFixtures.cpp)
target_compile_definitions(RackSynthRackLibraryProductTests PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1
    RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}" RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1")
target_link_libraries(RackSynthRackLibraryProductTests PRIVATE RackSynthProject RackSynthInstrument juce::juce_audio_utils juce::juce_cryptography juce::juce_dsp)
racksynth_add_test(NAME RackSynthRackLibraryProductTests_core COMMAND RackSynthRackLibraryProductTests --core
    --fixtures "${CMAKE_BINARY_DIR}/../rack-library-native/releases" --output "${CMAKE_BINARY_DIR}/../rack-library-native/task8/core")
set_tests_properties(RackSynthRackLibraryProductTests_core PROPERTIES RESOURCE_LOCK desktop)
racksynth_add_test(NAME RackSynthRackLibraryProductTests_cold_editor COMMAND RackSynthRackLibraryProductTests --cold-editor
    --fixtures "${CMAKE_BINARY_DIR}/../rack-library-native/releases" --output "${CMAKE_BINARY_DIR}/../rack-library-native/task9/cold-editor")
set_tests_properties(RackSynthRackLibraryProductTests_cold_editor PROPERTIES RESOURCE_LOCK desktop)
racksynth_add_test(NAME RackSynthRackLibraryProductTests_bundle COMMAND RackSynthRackLibraryProductTests
    --bundle "${CMAKE_BINARY_DIR}/RackSynthInstrument_artefacts/$<CONFIG>/VST3/AI Rack Synth.vst3"
    --fixtures "${CMAKE_BINARY_DIR}/../rack-library-native/releases" --output "${CMAKE_BINARY_DIR}/../rack-library-native/task8/bundle")
set_tests_properties(RackSynthRackLibraryProductTests_bundle PROPERTIES RESOURCE_LOCK desktop)
if(Python3_Interpreter_FOUND)
    add_custom_target(RackSynthRackPackageFixtures
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/rack_packages.py" build
            --source "${CMAKE_SOURCE_DIR}/racks" --output "${CMAKE_BINARY_DIR}/../rack-library-native/releases"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/rack_packages.py" verify
            --input "${CMAKE_BINARY_DIR}/../rack-library-native/releases"
        VERBATIM)
    add_dependencies(RackSynthRackLibraryProductTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthRackPackageTests RackSynthRackPackageFixtures)
endif()
juce_add_console_app(RackSynthReferencePluginProbe PRODUCT_NAME "RackSynthReferencePluginProbe")
target_sources(RackSynthReferencePluginProbe PRIVATE tools/native/ReferencePluginProbe.cpp)
target_compile_definitions(RackSynthReferencePluginProbe PRIVATE JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(RackSynthReferencePluginProbe PRIVATE juce::juce_audio_utils juce::juce_cryptography)
racksynth_add_test(NAME RackSynthReferencePluginProbeSelfTest COMMAND RackSynthReferencePluginProbe --self-test)
add_library(RackSynthLayout STATIC src/racks/RackLayout.cpp)
target_include_directories(RackSynthLayout PUBLIC src/racks)
add_executable(RackSynthLayoutTests tests/native/RackLayoutTests.cpp)
target_link_libraries(RackSynthLayoutTests PRIVATE RackSynthLayout)
racksynth_add_test(NAME RackSynthLayoutTests COMMAND RackSynthLayoutTests)

add_library(RackSynthRouted STATIC src/dsp/RoutedSynthEngine.cpp src/dsp/DspPipeline.cpp)
if(WIN32)
    target_link_libraries(RackSynthRouted PRIVATE Avrt)
endif()
target_link_libraries(RackSynthRouted PUBLIC RackSynthCore RackSynthLayout RackSynthJuce RackSynthDynamicEq RackSynthSaturation RackSynthEraReverb RackSynthFxMonitor RackSynthSpaceReverbControls RackSynthSpaceReverbActivation RackSynthDrumEngine)
if(WIN32)
    target_link_libraries(RackSynthRouted PRIVATE Synchronization)
endif()
add_executable(RackSynthDspPipelineTests tests/native/DspPipelineTests.cpp)
target_link_libraries(RackSynthDspPipelineTests PRIVATE RackSynthRouted)
racksynth_add_test(NAME RackSynthDspPipelineTests COMMAND RackSynthDspPipelineTests)
add_executable(RackSynthSynthParallelTests tests/native/SynthParallelTests.cpp)
target_link_libraries(RackSynthSynthParallelTests PRIVATE RackSynthRouted)
racksynth_add_test(NAME RackSynthSynthParallelTests COMMAND RackSynthSynthParallelTests)
add_library(RackSynthDynamicEq STATIC src/dsp/DynamicEq.cpp src/dsp/SpectralEq.cpp)
target_include_directories(RackSynthDynamicEq PUBLIC src/dsp)
target_link_libraries(RackSynthDynamicEq PUBLIC RackSynthJuce)
add_executable(RackSynthDynamicEqTests tests/native/DynamicEqTests.cpp)
target_link_libraries(RackSynthDynamicEqTests PRIVATE RackSynthDynamicEq)
racksynth_add_test(NAME RackSynthDynamicEqTests COMMAND RackSynthDynamicEqTests)
add_library(RackSynthEraReverb STATIC src/dsp/EraReverb.cpp)
target_include_directories(RackSynthEraReverb PUBLIC src/dsp)
add_executable(RackSynthEraReverbTests tests/native/EraReverbTests.cpp)
racksynth_add_test(NAME RackSynthEraReverbTests_analysis COMMAND RackSynthEraReverbTests --analysis)
racksynth_add_test(NAME RackSynthEraReverbTests_parts COMMAND RackSynthEraReverbTests --parts)
racksynth_add_test(NAME RackSynthEraReverbTests_core COMMAND RackSynthEraReverbTests --core)
racksynth_add_test(NAME RackSynthEraReverbTests_wobble COMMAND RackSynthEraReverbTests --wobble)
racksynth_add_test(NAME RackSynthEraReverbTests_attack COMMAND RackSynthEraReverbTests --attack)
racksynth_add_test(NAME RackSynthEraReverbTests_ages COMMAND RackSynthEraReverbTests --ages)
racksynth_add_test(NAME RackSynthEraReverbTests_grit COMMAND RackSynthEraReverbTests --grit)
racksynth_add_test(NAME RackSynthEraReverbTests_gate COMMAND RackSynthEraReverbTests --gate)
racksynth_add_test(NAME RackSynthEraReverbTests_runtime COMMAND RackSynthEraReverbTests --runtime)
racksynth_add_test(NAME RackSynthEraReverbTests_rates COMMAND RackSynthEraReverbTests --rates)
target_include_directories(RackSynthEraReverbTests PRIVATE src/dsp)
target_link_libraries(RackSynthEraReverbTests PRIVATE RackSynthEraReverb)
add_executable(RackSynthSpectralEqTests tests/native/SpectralEqTests.cpp)
target_link_libraries(RackSynthSpectralEqTests PRIVATE RackSynthDynamicEq)
racksynth_add_test(NAME RackSynthSpectralEqTests COMMAND RackSynthSpectralEqTests)
add_library(RackSynthFxMonitor STATIC src/dsp/FxMonitor.cpp)
target_include_directories(RackSynthFxMonitor PUBLIC src/dsp)
target_link_libraries(RackSynthFxMonitor PUBLIC RackSynthDynamicEq RackSynthJuce)
add_executable(RackSynthFxMonitorTests tests/native/FxMonitorTests.cpp)
target_link_libraries(RackSynthFxMonitorTests PRIVATE RackSynthFxMonitor RackSynthRouted)
racksynth_add_test(NAME RackSynthFxMonitorTests COMMAND RackSynthFxMonitorTests)
add_library(RackSynthFxSpectrum STATIC src/studio/FxSpectrum.cpp src/studio/SpectrumPeaks.cpp src/studio/FxAnalysisHub.cpp src/studio/EqReferenceAnalysis.cpp)
target_include_directories(RackSynthFxSpectrum PUBLIC src/studio src/state)
target_link_libraries(RackSynthFxSpectrum PUBLIC RackSynthFxMonitor RackSynthJuce PRIVATE RackSynthJson)
add_executable(RackSynthFxSpectrumTests tests/native/FxSpectrumTests.cpp)
target_link_libraries(RackSynthFxSpectrumTests PRIVATE RackSynthFxSpectrum RackSynthState)
racksynth_add_test(NAME RackSynthFxSpectrumTests COMMAND RackSynthFxSpectrumTests)
juce_add_console_app(RackSynthRoutedTests PRODUCT_NAME "RackSynthRoutedTests")
target_sources(RackSynthRoutedTests PRIVATE tests/native/RoutedSynthEngineTests.cpp)
target_link_libraries(RackSynthRoutedTests PRIVATE RackSynthRouted)
# Its groups run side by side in place of its long default run; RackSynthRoutedGroups holds them to that run.
set(racksynth_routed_groups native_prefix_head native_prefix_tail shared_dimension stereo flanger phaser chorus_eq_filter
    legacy_core modern_reverb modern_drive modern_compressor modern_modulated_taps modern_delay modern_eq modern_filter
    modern_frequency_split)
foreach(group IN LISTS racksynth_routed_groups)
    racksynth_add_test(NAME RackSynthRoutedTests_${group} COMMAND RackSynthRoutedTests)
    set_tests_properties(RackSynthRoutedTests_${group} PROPERTIES ENVIRONMENT "AIRACK_ROUTED_TEST_GROUP=${group}")
endforeach()
# Development listening/measurement renders straight from the routed engine.
juce_add_console_app(RackSynthAudition PRODUCT_NAME "RackSynthAudition")
target_sources(RackSynthAudition PRIVATE tools/native/AuditionVoices.cpp)
target_link_libraries(RackSynthAudition PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)

juce_add_console_app(RackSynthProfile PRODUCT_NAME "RackSynthProfile")
target_sources(RackSynthProfile PRIVATE tools/native/ProfileInstrument.cpp)
target_compile_definitions(RackSynthProfile PRIVATE
    RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}"
    RACKSYNTH_CONFIGURATION="$<CONFIG>")
target_link_libraries(RackSynthProfile PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
# Its linked-contention fixture observes real render-readiness and shutdown deadlines.
racksynth_add_test(NAME RackSynthProfileSelfTest EXCLUSIVE COMMAND RackSynthProfile --self-test)
add_executable(RackSynthHistoryTests tests/native/HistoryRingTests.cpp)
target_include_directories(RackSynthHistoryTests PRIVATE src/dsp)
racksynth_add_test(NAME RackSynthHistoryTests COMMAND RackSynthHistoryTests)

add_library(RackSynthPattern STATIC src/pattern/PatternDocument.cpp)
target_include_directories(RackSynthPattern PUBLIC src/pattern)
add_executable(RackSynthPatternTests tests/native/PatternDocumentTests.cpp)
target_link_libraries(RackSynthPatternTests PRIVATE RackSynthPattern)
racksynth_add_test(NAME RackSynthPatternTests COMMAND RackSynthPatternTests)

add_library(RackSynthPatternPlayback STATIC src/pattern/PatternTiming.cpp src/pattern/PatternScheduler.cpp)
target_include_directories(RackSynthPatternPlayback PUBLIC src/pattern)
target_link_libraries(RackSynthPatternPlayback PUBLIC RackSynthPattern)
add_library(RackSynthObservedMidi STATIC src/pattern/ObservedMidi.cpp)
target_link_libraries(RackSynthObservedMidi PUBLIC RackSynthPatternPlayback)
add_executable(RackSynthObservedMidiTests tests/native/ObservedMidiTests.cpp)
target_link_libraries(RackSynthObservedMidiTests PRIVATE RackSynthObservedMidi)
racksynth_add_test(NAME RackSynthObservedMidiTests COMMAND RackSynthObservedMidiTests)
juce_add_console_app(RackSynthPatternSchedulerTests PRODUCT_NAME "RackSynthPatternSchedulerTests")
target_sources(RackSynthPatternSchedulerTests PRIVATE tests/native/PatternSchedulerTests.cpp)
target_link_libraries(RackSynthPatternSchedulerTests PRIVATE RackSynthPatternPlayback RackSynthRouted)
racksynth_add_test(NAME RackSynthPatternSchedulerTests COMMAND RackSynthPatternSchedulerTests)
add_library(RackSynthPatternIO STATIC src/pattern/PatternIO.cpp)
target_link_libraries(RackSynthPatternIO PUBLIC RackSynthPatternPlayback RackSynthJuce)
juce_add_console_app(RackSynthPatternIOTests PRODUCT_NAME "RackSynthPatternIOTests")
target_sources(RackSynthPatternIOTests PRIVATE tests/native/PatternIOTests.cpp)
target_compile_definitions(RackSynthPatternIOTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthPatternIOTests PRIVATE RackSynthPatternIO)
racksynth_add_test(NAME RackSynthPatternIOTests COMMAND RackSynthPatternIOTests)
add_library(RackSynthProject STATIC src/state/ProjectDocument.cpp)
target_link_libraries(RackSynthProject PUBLIC RackSynthState RackSynthPatternIO)
add_library(RackSynthGroupPerformance STATIC src/state/GroupPerformance.cpp)
target_link_libraries(RackSynthGroupPerformance PUBLIC RackSynthProject RackSynthObservedMidi)
add_library(RackSynthGroupLinkState STATIC src/plugin/GroupLinkState.cpp)
target_include_directories(RackSynthGroupLinkState PUBLIC src/plugin)
target_link_libraries(RackSynthGroupLinkState PUBLIC RackSynthProject)
juce_add_console_app(RackSynthGroupLinkStateTests PRODUCT_NAME "RackSynthGroupLinkStateTests")
target_sources(RackSynthGroupLinkStateTests PRIVATE tests/native/GroupLinkStateTests.cpp)
target_link_libraries(RackSynthGroupLinkStateTests PRIVATE RackSynthGroupLinkState)
racksynth_add_test(NAME RackSynthGroupLinkStateTests COMMAND RackSynthGroupLinkStateTests)
juce_add_console_app(RackSynthProjectTests PRODUCT_NAME "RackSynthProjectTests")
target_sources(RackSynthProjectTests PRIVATE tests/native/ProjectDocumentTests.cpp)
target_compile_definitions(RackSynthProjectTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthProjectTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthProjectTests COMMAND RackSynthProjectTests)
# Extended tier: the group-wire corruption section alone, one bit position per test across every byte.
# Together they flip all eight bits of every byte; the gate's RackSynthProjectTests flips bit i%8 of byte i.
foreach(bit RANGE 7)
    racksynth_add_test(NAME RackSynthProjectTests_exhaustive_bit${bit} COMMAND RackSynthProjectTests)
    set_tests_properties(RackSynthProjectTests_exhaustive_bit${bit} PROPERTIES LABELS extended
        ENVIRONMENT "AIRACK_PROJECT_EXHAUSTIVE_CORRUPTION=${bit}")
endforeach()
target_link_libraries(RackSynthInstrument PUBLIC RackSynthProject)
target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentGroup.cpp src/plugin/InstrumentEqSpectrum.cpp)
target_link_libraries(RackSynthInstrument PUBLIC RackSynthGroupLinkState)
juce_add_console_app(RackSynthInstrumentGroupTests PRODUCT_NAME "RackSynthInstrumentGroupTests")
target_sources(RackSynthInstrumentGroupTests PRIVATE tests/native/InstrumentGroupTests.cpp)
target_compile_definitions(RackSynthInstrumentGroupTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthInstrumentGroupTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthInstrumentGroupTests COMMAND RackSynthInstrumentGroupTests)
juce_add_console_app(RackSynthInstrumentGroupWorkflowTests PRODUCT_NAME "RackSynthInstrumentGroupWorkflowTests")
target_sources(RackSynthInstrumentGroupWorkflowTests PRIVATE tests/native/InstrumentGroupWorkflowTests.cpp)
target_compile_definitions(RackSynthInstrumentGroupWorkflowTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}"
    RACKSYNTH_GROUP_RACK_FIXTURE_DIR="${CMAKE_BINARY_DIR}/../rack-library-native/releases")
target_link_libraries(RackSynthInstrumentGroupWorkflowTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthAiGeneration RackSynthCompanionBackend)
# The four-peer fixture observes the real five-second generation/response deadlines.
racksynth_add_test(NAME RackSynthInstrumentGroupWorkflowTests EXCLUSIVE COMMAND RackSynthInstrumentGroupWorkflowTests)
juce_add_console_app(RackSynthInstrumentPatternTests PRODUCT_NAME "RackSynthInstrumentPatternTests")
target_sources(RackSynthInstrumentPatternTests PRIVATE tests/native/InstrumentPatternTests.cpp)
target_compile_definitions(RackSynthInstrumentPatternTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthInstrumentPatternTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthInstrumentPatternTests COMMAND RackSynthInstrumentPatternTests)
target_sources(RackSynthInstrument PRIVATE src/plugin/PerformanceMidi.cpp)
target_link_libraries(RackSynthInstrument PUBLIC RackSynthObservedMidi)
juce_add_console_app(RackSynthInstrumentObservedMidiTests PRODUCT_NAME "RackSynthInstrumentObservedMidiTests")
target_sources(RackSynthInstrumentObservedMidiTests PRIVATE tests/native/InstrumentObservedMidiTests.cpp)
target_compile_definitions(RackSynthInstrumentObservedMidiTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthInstrumentObservedMidiTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthInstrumentObservedMidiTests COMMAND RackSynthInstrumentObservedMidiTests)
juce_add_console_app(RackSynthPatternEditorTests PRODUCT_NAME "RackSynthPatternEditorTests")
target_sources(RackSynthPatternEditorTests PRIVATE tests/native/PatternEditorTests.cpp tests/native/PostedClick.cpp)
target_link_libraries(RackSynthPatternEditorTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthPatternEditorTests COMMAND RackSynthPatternEditorTests)
target_compile_definitions(RackSynthPatternEditorTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")

add_library(RackSynthAiProposal STATIC src/ai/ProposalDocument.cpp src/ai/AiCatalog.cpp)
target_link_libraries(RackSynthAiProposal PUBLIC RackSynthProject RackSynthJson RackSynthRackInventory)
juce_add_console_app(RackSynthAiProposalTests PRODUCT_NAME "RackSynthAiProposalTests")
target_sources(RackSynthAiProposalTests PRIVATE tests/native/AiProposalTests.cpp)
target_link_libraries(RackSynthAiProposalTests PRIVATE RackSynthAiProposal)
racksynth_add_test(NAME RackSynthAiProposalTests COMMAND RackSynthAiProposalTests)

add_library(RackSynthAiRender STATIC src/ai/PreviewRender.cpp)
target_include_directories(RackSynthAiRender PUBLIC src/ai)
target_link_libraries(RackSynthAiRender PUBLIC RackSynthProject RackSynthRouted RackSynthJuce)
juce_add_console_app(RackSynthAiRenderTests PRODUCT_NAME "RackSynthAiRenderTests")
target_sources(RackSynthAiRenderTests PRIVATE tests/native/PreviewRenderTests.cpp)
target_link_libraries(RackSynthAiRenderTests PRIVATE RackSynthAiRender)
racksynth_add_test(NAME RackSynthAiRenderTests COMMAND RackSynthAiRenderTests)
if(WIN32)
    add_library(RackSynthAiGeneration STATIC src/ai/GenerationRequest.cpp)
    target_link_libraries(RackSynthAiGeneration PUBLIC RackSynthAiProposal RackSynthAiRender RackSynthOAuthCore RackSynthGroupPerformance RackSynthGenerationLog)
    target_link_libraries(RackSynthRackLibraryProductTests PRIVATE RackSynthAiGeneration)
    juce_add_console_app(RackSynthAiGenerationTests PRODUCT_NAME "RackSynthAiGenerationTests")
    target_sources(RackSynthAiGenerationTests PRIVATE tests/native/GenerationRequestTests.cpp)
    target_link_libraries(RackSynthAiGenerationTests PRIVATE RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthAiGenerationTests COMMAND RackSynthAiGenerationTests)
endif()

# Pure bounded companion protocol; transport/runtime remain separate from the instrument.
if(WIN32)
    add_library(RackSynthCompanionWire STATIC src/companion/CompanionWire.cpp src/companion/GroupSpectrum.cpp)
    target_include_directories(RackSynthCompanionWire PUBLIC src/companion src/ai probes/oauth)
    target_link_libraries(RackSynthCompanionWire PUBLIC RackSynthAiProposal RackSynthGroupPerformance)
    juce_add_console_app(RackSynthGroupWireTests PRODUCT_NAME "RackSynthGroupWireTests")
    target_sources(RackSynthGroupWireTests PRIVATE tests/native/GroupWireTests.cpp)
    target_link_libraries(RackSynthGroupWireTests PRIVATE RackSynthCompanionWire)
    racksynth_add_test(NAME RackSynthGroupWireTests COMMAND RackSynthGroupWireTests)
    add_library(RackSynthGroupRegistry STATIC src/companion/GroupRegistry.cpp)
    target_link_libraries(RackSynthGroupRegistry PUBLIC RackSynthCompanionWire)
    juce_add_console_app(RackSynthGroupRegistryTests PRODUCT_NAME "RackSynthGroupRegistryTests")
    target_sources(RackSynthGroupRegistryTests PRIVATE tests/native/GroupRegistryTests.cpp)
    target_link_libraries(RackSynthGroupRegistryTests PRIVATE RackSynthGroupRegistry)
    racksynth_add_test(NAME RackSynthGroupRegistryTests COMMAND RackSynthGroupRegistryTests)
    target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentAi.cpp)
    target_link_libraries(RackSynthInstrument PUBLIC RackSynthCompanionWire)
    juce_add_console_app(RackSynthInstrumentAiTests PRODUCT_NAME "RackSynthInstrumentAiTests")
    target_sources(RackSynthInstrumentAiTests PRIVATE tests/native/InstrumentAiTests.cpp tests/native/RackPackageFixtures.cpp)
    target_compile_definitions(RackSynthInstrumentAiTests PRIVATE RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1")
    target_compile_definitions(RackSynthInstrumentAiTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthInstrumentAiTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthInstrumentAiTests COMMAND RackSynthInstrumentAiTests)
    juce_add_console_app(RackSynthInstrumentAiGestureTests PRODUCT_NAME "RackSynthInstrumentAiGestureTests")
    target_sources(RackSynthInstrumentAiGestureTests PRIVATE tests/native/InstrumentAiGestureTests.cpp)
    target_link_libraries(RackSynthInstrumentAiGestureTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthInstrumentAiGestureTests COMMAND RackSynthInstrumentAiGestureTests)
    target_sources(RackSynthInstrument PRIVATE src/plugin/RenderedPreview.cpp)
    juce_add_console_app(RackSynthRenderedPreviewTests PRODUCT_NAME "RackSynthRenderedPreviewTests")
    target_sources(RackSynthRenderedPreviewTests PRIVATE tests/native/RenderedPreviewTests.cpp)
    target_compile_definitions(RackSynthRenderedPreviewTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthRenderedPreviewTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthRenderedPreviewTests COMMAND RackSynthRenderedPreviewTests)
    juce_add_console_app(RackSynthCompanionWireTests PRODUCT_NAME "RackSynthCompanionWireTests")
    target_sources(RackSynthCompanionWireTests PRIVATE tests/native/CompanionWireTests.cpp)
    target_link_libraries(RackSynthCompanionWireTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthCompanionWireTests COMMAND RackSynthCompanionWireTests)

    add_executable(RackSynthDrumRegistryStateTests tests/native/DrumRegistryStateTests.cpp)
    target_link_libraries(RackSynthDrumRegistryStateTests PRIVATE RackSynthCompanionWire)
    target_compile_definitions(RackSynthDrumRegistryStateTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    racksynth_add_test(NAME RackSynthDrumRegistryStateTests COMMAND RackSynthDrumRegistryStateTests)
    add_executable(RackSynthDrumMainReconciliationTests tests/native/DrumMainReconciliationTests.cpp)
    target_link_libraries(RackSynthDrumMainReconciliationTests PRIVATE RackSynthCompanionWire)
    target_compile_definitions(RackSynthDrumMainReconciliationTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    racksynth_add_test(NAME RackSynthDrumMainReconciliationTests COMMAND RackSynthDrumMainReconciliationTests)
endif()

# Current-user native transport; the runtime and instrument remain separate.
if(WIN32)
    add_library(RackSynthCompanionPipe STATIC src/companion/CompanionPipe.cpp)
    target_link_libraries(RackSynthCompanionPipe PUBLIC RackSynthCompanionWire Advapi32)
    juce_add_console_app(RackSynthCompanionPipeTests PRODUCT_NAME "RackSynthCompanionPipeTests")
    target_sources(RackSynthCompanionPipeTests PRIVATE tests/native/CompanionPipeTests.cpp)
    target_link_libraries(RackSynthCompanionPipeTests PRIVATE RackSynthCompanionPipe)
    racksynth_add_test(NAME RackSynthCompanionPipeTests COMMAND RackSynthCompanionPipeTests)
endif()

# Bounded logical session/job ownership; native IO and providers are later owners.
if(WIN32)
    add_library(RackSynthCompanionRuntimeState STATIC src/companion/CompanionRuntimeState.cpp src/companion/LocalPreviewExchange.cpp)
    target_link_libraries(RackSynthCompanionRuntimeState PUBLIC RackSynthCompanionWire RackSynthGroupRegistry RackSynthGroupPerformance)
    juce_add_console_app(RackSynthCompanionGroupRuntimeTests PRODUCT_NAME "RackSynthCompanionGroupRuntimeTests")
    target_sources(RackSynthCompanionGroupRuntimeTests PRIVATE tests/native/CompanionGroupRuntimeTests.cpp)
    target_link_libraries(RackSynthCompanionGroupRuntimeTests PRIVATE RackSynthCompanionRuntimeState)
    racksynth_add_test(NAME RackSynthCompanionGroupRuntimeTests COMMAND RackSynthCompanionGroupRuntimeTests)
    juce_add_console_app(RackSynthCompanionRuntimeStateTests PRODUCT_NAME "RackSynthCompanionRuntimeStateTests")
    target_sources(RackSynthCompanionRuntimeStateTests PRIVATE tests/native/CompanionRuntimeStateTests.cpp)
    target_link_libraries(RackSynthCompanionRuntimeStateTests PRIVATE RackSynthCompanionRuntimeState RackSynthAiRender)
    racksynth_add_test(NAME RackSynthCompanionRuntimeStateTests COMMAND RackSynthCompanionRuntimeStateTests)
endif()

# Native IO owner and checked serial worker, separate from the instrument/backend.
if(WIN32)
    add_library(RackSynthCompanionRuntimeServer STATIC src/companion/CompanionRuntimeServer.cpp)
    target_link_libraries(RackSynthCompanionRuntimeServer PUBLIC RackSynthCompanionRuntimeState RackSynthCompanionPipe)
    juce_add_console_app(RackSynthCompanionRuntimeServerTests PRODUCT_NAME "RackSynthCompanionRuntimeServerTests")
    target_sources(RackSynthCompanionRuntimeServerTests PRIVATE tests/native/CompanionRuntimeServerTests.cpp)
    target_link_libraries(RackSynthCompanionRuntimeServerTests PRIVATE RackSynthCompanionRuntimeServer RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthCompanionRuntimeServerTests COMMAND RackSynthCompanionRuntimeServerTests)
endif()

# Concrete native account/generation adapter; intentionally unlinked from the instrument.
if(WIN32)
    add_library(RackSynthCompanionBackend STATIC src/companion/CompanionBackend.cpp)
    target_link_libraries(RackSynthCompanionBackend PUBLIC RackSynthCompanionRuntimeState RackSynthAiGeneration RackSynthOAuthCore)
    juce_add_console_app(RackSynthCompanionBackendTests PRODUCT_NAME "RackSynthCompanionBackendTests")
    target_sources(RackSynthCompanionBackendTests PRIVATE tests/native/CompanionBackendTests.cpp)
    target_link_libraries(RackSynthCompanionBackendTests PRIVATE RackSynthCompanionBackend RackSynthCompanionRuntimeServer bcrypt)
    racksynth_add_test(NAME RackSynthCompanionBackendTests COMMAND RackSynthCompanionBackendTests)
    juce_add_console_app(RackSynthGroupGenerationTests PRODUCT_NAME "RackSynthGroupGenerationTests")
    target_sources(RackSynthGroupGenerationTests PRIVATE tests/native/GroupGenerationTests.cpp)
    target_link_libraries(RackSynthGroupGenerationTests PRIVATE RackSynthCompanionBackend)
    racksynth_add_test(NAME RackSynthGroupGenerationTests COMMAND RackSynthGroupGenerationTests)
endif()

# Hidden native helper and process handles; still unlinked from the instrument.
if(WIN32)
    add_library(RackSynthCompanionLaunch STATIC src/companion/CompanionLaunch.cpp)
    target_link_libraries(RackSynthCompanionLaunch PUBLIC RackSynthCompanionPipe RackSynthGenerationLog PRIVATE shell32 ole32)
    add_library(RackSynthMcpBridge STATIC src/mcp/McpBridge.cpp)
    target_link_libraries(RackSynthMcpBridge PUBLIC RackSynthMcpProtocol RackSynthCompanionLaunch RackSynthCompanionPipe)
    add_library(RackSynthMcpService STATIC src/mcp/McpService.cpp)
    target_link_libraries(RackSynthMcpService PUBLIC RackSynthMcpProtocol RackSynthCompanionPipe)
    add_library(RackSynthMcpEndpoint STATIC src/mcp/McpEndpoint.cpp)
    target_link_libraries(RackSynthMcpEndpoint PUBLIC RackSynthMcpProtocol RackSynthCompanionLaunch)
    target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentMcp.cpp src/plugin/InstrumentMcpPreview.cpp src/plugin/InstrumentProposal.cpp)
    juce_add_console_app(RackSynthInstrumentMcpTests PRODUCT_NAME "RackSynthInstrumentMcpTests")
    target_sources(RackSynthInstrumentMcpTests PRIVATE tests/native/InstrumentMcpTests.cpp)
    target_link_libraries(RackSynthInstrumentMcpTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
    racksynth_add_test(NAME RackSynthInstrumentMcpTests COMMAND RackSynthInstrumentMcpTests)
    racksynth_add_test(NAME RackSynthInstrumentMcpTests_inventory_scale COMMAND RackSynthInstrumentMcpTests --inventory-scale)
    juce_add_console_app(RackSynthInstrumentMcpPreviewTests PRODUCT_NAME "RackSynthInstrumentMcpPreviewTests")
    target_sources(RackSynthInstrumentMcpPreviewTests PRIVATE tests/native/InstrumentMcpPreviewTests.cpp)
    target_link_libraries(RackSynthInstrumentMcpPreviewTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
    racksynth_add_test(NAME RackSynthInstrumentMcpPreviewTests COMMAND RackSynthInstrumentMcpPreviewTests)
    target_link_libraries(RackSynthInstrument PUBLIC RackSynthMcpEndpoint RackSynthMcpProtocol)
    juce_add_console_app(RackSynthMcpServiceTests PRODUCT_NAME "RackSynthMcpServiceTests")
    target_sources(RackSynthMcpServiceTests PRIVATE tests/native/McpServiceTests.cpp)
    target_link_libraries(RackSynthMcpServiceTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthMcpService RackSynthMcpEndpoint RackSynthCompanionRuntimeState RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthMcpServiceTests COMMAND RackSynthMcpServiceTests $<TARGET_FILE:RackSynthCompanion> --instances 16)
    set_tests_properties(RackSynthMcpServiceTests PROPERTIES PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS})
    juce_add_console_app(RackSynthCompanion PRODUCT_NAME "AI Rack Synth Companion")
    target_sources(RackSynthCompanion PRIVATE src/companion/CompanionMain.cpp)
    add_dependencies(RackSynthMcpServiceTests RackSynthCompanion)
    target_compile_definitions(RackSynthMcpProtocolTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    add_dependencies(RackSynthMcpProtocolTests RackSynthCompanion)
    target_link_libraries(RackSynthCompanion PRIVATE RackSynthMcpService RackSynthMcpBridge RackSynthCompanionLaunch RackSynthCompanionRuntimeServer RackSynthCompanionBackend shell32 ole32)
    set_target_properties(RackSynthCompanion PROPERTIES OUTPUT_NAME "AI Rack Synth Companion")
    add_library(RackSynthCompanionModuleFixture SHARED tests/native/CompanionModuleFixture.cpp)
    target_link_libraries(RackSynthCompanionModuleFixture PRIVATE RackSynthCompanionLaunch)
    set_target_properties(RackSynthCompanionModuleFixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/companion-module-fixture")
    juce_add_console_app(RackSynthCompanionLaunchTests PRODUCT_NAME "RackSynthCompanionLaunchTests")
    target_sources(RackSynthCompanionLaunchTests PRIVATE tests/native/CompanionLaunchTests.cpp)
    target_link_libraries(RackSynthCompanionLaunchTests PRIVATE RackSynthCompanionLaunch RackSynthCompanionBackend RackSynthCompanionRuntimeServer shell32 ole32)
    target_compile_definitions(RackSynthCompanionLaunchTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    add_dependencies(RackSynthCompanionLaunchTests RackSynthCompanion RackSynthCompanionModuleFixture)
    racksynth_add_test(NAME RackSynthCompanionLaunchTests COMMAND RackSynthCompanionLaunchTests $<TARGET_FILE:RackSynthCompanion> $<TARGET_FILE:RackSynthCompanionModuleFixture>)
endif()

# Caller-pumped client, separate from product and provider libraries.
if(WIN32)
    add_library(RackSynthCompanionClient STATIC src/companion/CompanionClient.cpp)
    target_link_libraries(RackSynthCompanionClient PUBLIC RackSynthCompanionLaunch)
    target_link_libraries(RackSynthInstrument PUBLIC RackSynthCompanionClient)
    juce_add_console_app(RackSynthAiPanelTests PRODUCT_NAME "RackSynthAiPanelTests")
    target_sources(RackSynthAiPanelTests PRIVATE tests/native/AiPanelTests.cpp tests/native/PostedClick.cpp)
    target_compile_definitions(RackSynthAiPanelTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthAiPanelTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthAiPanelTests COMMAND RackSynthAiPanelTests)
    juce_add_console_app(RackSynthMcpAccessTests PRODUCT_NAME "RackSynthMcpAccessTests")
    target_sources(RackSynthMcpAccessTests PRIVATE tests/native/McpAccessTests.cpp tests/native/PostedClick.cpp)
    target_compile_definitions(RackSynthMcpAccessTests PRIVATE RACKSYNTH_UI_HELPER="$<TARGET_FILE:RackSynthCompanion>")
    add_dependencies(RackSynthMcpAccessTests RackSynthCompanion)
    target_link_libraries(RackSynthMcpAccessTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
    racksynth_add_test(NAME RackSynthMcpAccessTests COMMAND RackSynthMcpAccessTests)
    set_tests_properties(RackSynthMcpAccessTests PROPERTIES RESOURCE_LOCK desktop)
    juce_add_console_app(RackSynthMcpPluginWorkflowTests PRODUCT_NAME "RackSynthMcpPluginWorkflowTests")
    target_sources(RackSynthMcpPluginWorkflowTests PRIVATE tests/native/McpPluginWorkflowTests.cpp)
    target_compile_definitions(RackSynthMcpPluginWorkflowTests PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        RACKSYNTH_MCP_VST3="${CMAKE_BINARY_DIR}/RackSynthInstrument_artefacts/$<CONFIG>/VST3/AI Rack Synth.vst3"
        RACKSYNTH_MCP_HELPER="$<TARGET_FILE:RackSynthCompanion>")
    target_link_libraries(RackSynthMcpPluginWorkflowTests PRIVATE RackSynthState RackSynthMcpProtocol RackSynthCompanionLaunch RackSynthRackPackageStore juce::juce_audio_utils juce::juce_cryptography)
    add_dependencies(RackSynthMcpPluginWorkflowTests RackSynthCompanionDeploy)
    racksynth_add_test(NAME RackSynthMcpPluginWorkflowTests COMMAND RackSynthMcpPluginWorkflowTests)
    set_tests_properties(RackSynthMcpPluginWorkflowTests PROPERTIES PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS})
    juce_add_console_app(RackSynthGroupPanelTests PRODUCT_NAME "RackSynthGroupPanelTests")
    target_sources(RackSynthGroupPanelTests PRIVATE tests/native/GroupPanelTests.cpp tests/native/PostedClick.cpp)
    target_compile_definitions(RackSynthGroupPanelTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthGroupPanelTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthGroupPanelTests COMMAND RackSynthGroupPanelTests)
    juce_add_console_app(RackSynthAiPanelRackDiffTests PRODUCT_NAME "RackSynthAiPanelRackDiffTests")
    target_sources(RackSynthAiPanelRackDiffTests PRIVATE tests/native/AiPanelRackDiffTests.cpp tests/native/PostedClick.cpp)
    target_link_libraries(RackSynthAiPanelRackDiffTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthAiPanelRackDiffTests COMMAND RackSynthAiPanelRackDiffTests)
    juce_add_console_app(RackSynthCompanionClientTests PRODUCT_NAME "RackSynthCompanionClientTests")
    target_sources(RackSynthCompanionClientTests PRIVATE tests/native/CompanionClientTests.cpp)
    target_link_libraries(RackSynthCompanionClientTests PRIVATE RackSynthCompanionClient RackSynthCompanionRuntimeServer RackSynthAiGeneration)
    target_compile_definitions(RackSynthCompanionClientTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    add_dependencies(RackSynthCompanionClientTests RackSynthCompanion RackSynthCompanionModuleFixture)
    racksynth_add_test(NAME RackSynthCompanionClientTests COMMAND RackSynthCompanionClientTests $<TARGET_FILE:RackSynthCompanion> $<TARGET_FILE:RackSynthCompanionModuleFixture> $<TARGET_FILE_DIR:RackSynthInstrument_VST3>)
    juce_add_console_app(RackSynthCompanionGroupClientTests PRODUCT_NAME "RackSynthCompanionGroupClientTests")
    target_sources(RackSynthCompanionGroupClientTests PRIVATE tests/native/CompanionGroupClientTests.cpp)
    target_link_libraries(RackSynthCompanionGroupClientTests PRIVATE RackSynthCompanionClient RackSynthCompanionRuntimeServer RackSynthAiRender)
    target_compile_definitions(RackSynthCompanionGroupClientTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    racksynth_add_test(NAME RackSynthCompanionGroupClientTests COMMAND RackSynthCompanionGroupClientTests)
    add_custom_target(RackSynthCompanionDeploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RackSynthCompanion>" "$<TARGET_FILE_DIR:RackSynthInstrument_VST3>/AI Rack Synth Companion.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RackSynthCompanion>" "$<TARGET_FILE_DIR:RackSynthInstrument_Standalone>/AI Rack Synth Companion.exe"
        DEPENDS RackSynthCompanion RackSynthInstrument_VST3 RackSynthInstrument_Standalone
        VERBATIM)
endif()
# Developer public Client-only proof; intentionally unlinked from the instrument.
if(WIN32 AND RACKSYNTH_BUILD_PROBES)
    juce_add_console_app(RackSynthCompanionAccountProbe PRODUCT_NAME "RackSynthCompanionAccountProbe")
    target_sources(RackSynthCompanionAccountProbe PRIVATE tools/native/CompanionAccountProbe.cpp)
    target_compile_definitions(RackSynthCompanionAccountProbe PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthCompanionAccountProbe PRIVATE RackSynthCompanionClient RackSynthAiProposal RackSynthAiRender RackSynthJuce shell32)
    add_custom_target(RackSynthCompanionAccountProbeDeploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RackSynthCompanion>" "$<TARGET_FILE_DIR:RackSynthCompanionAccountProbe>/AI Rack Synth Companion.exe"
        DEPENDS RackSynthCompanion RackSynthCompanionAccountProbe VERBATIM)
    juce_add_console_app(RackSynthCompanionAccountProbeTests PRODUCT_NAME "RackSynthCompanionAccountProbeTests")
    target_sources(RackSynthCompanionAccountProbeTests PRIVATE tests/native/CompanionAccountProbeTests.cpp)
    target_compile_definitions(RackSynthCompanionAccountProbeTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthCompanionAccountProbeTests PRIVATE RackSynthCompanionRuntimeServer RackSynthAiGeneration RackSynthJuce shell32)
    add_dependencies(RackSynthCompanionAccountProbeTests RackSynthCompanionAccountProbeDeploy)
    racksynth_add_test(NAME RackSynthCompanionAccountProbeTests COMMAND RackSynthCompanionAccountProbeTests $<TARGET_FILE:RackSynthCompanionAccountProbe> $<TARGET_FILE:RackSynthCompanion>)
    set_tests_properties(RackSynthCompanionAccountProbeTests PROPERTIES PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS})
endif()

# Development-only processor/editor workflow acceptance; product linkage unchanged.
if(WIN32 AND RACKSYNTH_BUILD_PROBES)
    juce_add_console_app(RackSynthAiWorkflow PRODUCT_NAME "RackSynthAiWorkflow")
    target_sources(RackSynthAiWorkflow PRIVATE tools/native/AiWorkflow.cpp)
    target_compile_definitions(RackSynthAiWorkflow PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_include_directories(RackSynthAiWorkflow PRIVATE tests/native)
    target_link_libraries(RackSynthAiWorkflow PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthAiGeneration RackSynthCompanionBackend)
    racksynth_add_test(NAME RackSynthAiWorkflowSelfTest COMMAND RackSynthAiWorkflow --self-test)
    juce_add_console_app(RackSynthGroupWorkflow PRODUCT_NAME "RackSynthGroupWorkflow")
    target_sources(RackSynthGroupWorkflow PRIVATE tools/native/GroupWorkflow.cpp)
    target_compile_definitions(RackSynthGroupWorkflow PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_include_directories(RackSynthGroupWorkflow PRIVATE tests/native)
    target_link_libraries(RackSynthGroupWorkflow PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthAiGeneration)
    # This live two-editor protocol check observes the real five-second group deadline.
    racksynth_add_test(NAME RackSynthGroupWorkflowSelfTest EXCLUSIVE COMMAND RackSynthGroupWorkflow --self-test)
endif()

# Existing profiler's optional development-only linked production workload.
if(WIN32 AND RACKSYNTH_BUILD_PROBES)
    target_compile_definitions(RackSynthProfile PRIVATE RACKSYNTH_PRODUCTION_PROFILE=1)
    target_link_libraries(RackSynthProfile PRIVATE RackSynthCompanionRuntimeServer RackSynthAiRender RackSynthAiGeneration)
    racksynth_add_test(NAME RackSynthDrumLocalProfileCallbackRetention COMMAND RackSynthCompanionRuntimeServerTests --drum-local-profile-retention $<TARGET_FILE:RackSynthProfile>)
    racksynth_add_test(NAME RackSynthDrumLocalCoverageSelfTest COMMAND RackSynthProfile --drum-local-coverage-self-test)
endif()

# Native focused rack personality GUI checks.
juce_add_console_app(RackSynthRackPanelTests PRODUCT_NAME "RackSynthRackPanelTests")
target_sources(RackSynthRackPanelTests PRIVATE tests/native/RackPanelTests.cpp tests/native/PostedClick.cpp)
target_link_libraries(RackSynthRackPanelTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthRackPanelTests COMMAND RackSynthRackPanelTests --only non-focus)
racksynth_add_test(NAME RackSynthRackPanelTests_focus COMMAND RackSynthRackPanelTests --only focus)
# Native studio window checks.
juce_add_console_app(RackSynthRackLibraryUiTests PRODUCT_NAME "RackSynthRackLibraryUiTests")
target_sources(RackSynthRackLibraryUiTests PRIVATE tests/native/RackLibraryUiTests.cpp tests/native/RackPackageFixtures.cpp tests/native/PostedClick.cpp)
target_compile_definitions(RackSynthRackLibraryUiTests PRIVATE RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1" RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthRackLibraryUiTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthRackLibraryUiTests COMMAND RackSynthRackLibraryUiTests)
set_tests_properties(RackSynthRackLibraryUiTests PROPERTIES RESOURCE_LOCK desktop)
juce_add_console_app(RackSynthStudioWindowTests PRODUCT_NAME "RackSynthStudioWindowTests")
target_sources(RackSynthStudioWindowTests PRIVATE tests/native/StudioWindowTests.cpp tests/native/PostedClick.cpp)
target_sources(RackSynthStudioWindowTests PRIVATE src/racks/RackLibraryClient.cpp)
target_compile_definitions(RackSynthStudioWindowTests PRIVATE AIRACK_LIBRARY_CACHE_TESTS=1)
target_compile_definitions(RackSynthStudioWindowTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthStudioWindowTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthStudioWindowTests_units COMMAND RackSynthStudioWindowTests --partition units)
racksynth_add_test(NAME RackSynthStudioWindowTests_stack COMMAND RackSynthStudioWindowTests --partition stack)
racksynth_add_test(NAME RackSynthStudioWindowTests_window COMMAND RackSynthStudioWindowTests --partition window)
set_tests_properties(RackSynthStudioWindowTests_stack PROPERTIES DEPENDS RackSynthStudioWindowTests_units)
set_tests_properties(RackSynthStudioWindowTests_window PROPERTIES DEPENDS RackSynthStudioWindowTests_stack)
racksynth_add_test(NAME RackSynthStudioWindowTests_rack_library_preparation COMMAND RackSynthStudioWindowTests --only stackLibraryPreparation)
set_tests_properties(RackSynthStudioWindowTests_rack_library_preparation PROPERTIES RESOURCE_LOCK desktop)
# Native maker faceplate checks: the makers' skins, the designs they face and the faceplates over the real processor.
juce_add_console_app(RackSynthRackPackageRecallTests PRODUCT_NAME "RackSynthRackPackageRecallTests")
target_sources(RackSynthRackPackageRecallTests PRIVATE tests/native/RackPackageRecallTests.cpp tests/native/RackPackageFixtures.cpp)
target_compile_definitions(RackSynthRackPackageRecallTests PRIVATE RACKSYNTH_RECALL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-package-recall")
target_compile_definitions(RackSynthRackPackageRecallTests PRIVATE RACKSYNTH_PACKAGE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/rack-library/v1" RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthRackPackageRecallTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthRackPackageRecallTests COMMAND RackSynthRackPackageRecallTests "${CMAKE_BINARY_DIR}/../rack-library-native/task4/trials")
juce_add_console_app(RackSynthMakerFaceTests PRODUCT_NAME "RackSynthMakerFaceTests")
target_sources(RackSynthMakerFaceTests PRIVATE tests/native/MakerFaceTests.cpp)
target_link_libraries(RackSynthMakerFaceTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
if(TARGET RackSynthRackPackageFixtures)
    if(TARGET RackSynthInstrumentMcpTests)
        add_dependencies(RackSynthInstrumentMcpTests RackSynthRackPackageFixtures)
    endif()
    add_dependencies(RackSynthAiPanelTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthInstrumentTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthMakerFaceTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthStudioWindowTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthRackPanelTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthRackPackageRecallTests RackSynthRackPackageFixtures)
    add_dependencies(RackSynthInstrumentGroupWorkflowTests RackSynthRackPackageFixtures)
endif()
racksynth_add_test(NAME RackSynthMakerFaceTests COMMAND RackSynthMakerFaceTests)

# Native PROGRAMMER checks: its values, knobs, pages and RACK LAYOUT over the real processor.
juce_add_console_app(RackSynthProgrammerTests PRODUCT_NAME "RackSynthProgrammerTests")
target_sources(RackSynthProgrammerTests PRIVATE tests/native/ProgrammerTests.cpp)
target_compile_definitions(RackSynthProgrammerTests PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthProgrammerTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthProgrammerTests COMMAND RackSynthProgrammerTests)

# Each JUCE module source builds once per set of definitions, read from the generated Visual Studio projects.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    racksynth_add_test(NAME RackSynthJuceModuleBuilds COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/juce_module_builds.py"
        --build-dir "${CMAKE_BINARY_DIR}" --juce-modules "${juce_SOURCE_DIR}/modules" --config $<CONFIG>)
endif()
if(Python3_Interpreter_FOUND)
    racksynth_add_test(NAME RackSynthMcpCodexProbeTests COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_SOURCE_DIR}/tests" -p test_mcp_codex_probe.py)
    if(WIN32)
        racksynth_add_test(NAME RackSynthJuceGraphicsPatch COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_juce_graphics_patch.py"
            --cmake "${CMAKE_COMMAND}" --juce-modules "${juce_SOURCE_DIR}/modules")
    endif()
    racksynth_add_test(NAME RackSynthRoutedGroups COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/routed_groups.py"
        --source "${CMAKE_SOURCE_DIR}/tests/native/RoutedSynthEngineTests.cpp" ${racksynth_routed_groups})
endif()

juce_add_console_app(RackSynthFreezeSaturationLegacy PRODUCT_NAME "RackSynthFreezeSaturationLegacy")
juce_add_console_app(RackSynthSaturationReleaseFixtures PRODUCT_NAME "RackSynthSaturationReleaseFixtures")
target_sources(RackSynthSaturationReleaseFixtures PRIVATE tests/native/SaturationReleaseFixtures.cpp)
target_compile_definitions(RackSynthSaturationReleaseFixtures PRIVATE RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
target_link_libraries(RackSynthSaturationReleaseFixtures PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthSaturationReleaseFixtures COMMAND RackSynthSaturationReleaseFixtures --self-test)
target_sources(RackSynthFreezeSaturationLegacy PRIVATE tools/native/FreezeSaturationLegacy.cpp)
target_link_libraries(RackSynthFreezeSaturationLegacy PRIVATE RackSynthProject)
juce_add_console_app(RackSynthSaturationStateTests PRODUCT_NAME "RackSynthSaturationStateTests")
target_sources(RackSynthSaturationStateTests PRIVATE tests/native/SaturationStateTests.cpp)
target_compile_definitions(RackSynthSaturationStateTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/saturation-legacy")
target_link_libraries(RackSynthSaturationStateTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthSaturationStateTests COMMAND RackSynthSaturationStateTests)

juce_add_console_app(RackSynthMainSyncStateTests PRODUCT_NAME "RackSynthMainSyncStateTests")
target_sources(RackSynthMainSyncStateTests PRIVATE tests/native/MainSyncStateTests.cpp)
target_compile_definitions(RackSynthMainSyncStateTests PRIVATE RACKSYNTH_MAIN_SYNC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/main-sync")
target_link_libraries(RackSynthMainSyncStateTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthMainSyncStateTests COMMAND RackSynthMainSyncStateTests)

juce_add_console_app(RackSynthSaturationPendingTests PRODUCT_NAME "RackSynthSaturationPendingTests")
target_sources(RackSynthSaturationPendingTests PRIVATE tests/native/SaturationPendingTests.cpp)
target_link_libraries(RackSynthSaturationPendingTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthSaturationPendingTests COMMAND RackSynthSaturationPendingTests)
juce_add_console_app(RackSynthSaturationRoutingTests PRODUCT_NAME "RackSynthSaturationRoutingTests")
target_sources(RackSynthSaturationRoutingTests PRIVATE tests/native/SaturationRoutingTests.cpp)
target_link_libraries(RackSynthSaturationRoutingTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthSaturationRoutingTests COMMAND RackSynthSaturationRoutingTests)

juce_add_console_app(RackSynthFreezeDynamicEqLegacy PRODUCT_NAME "RackSynthFreezeDynamicEqLegacy")
target_sources(RackSynthFreezeDynamicEqLegacy PRIVATE tools/native/FreezeDynamicEqLegacy.cpp)
target_link_libraries(RackSynthFreezeDynamicEqLegacy PRIVATE RackSynthProject)

juce_add_console_app(RackSynthDynamicEqIntegrationTests PRODUCT_NAME "RackSynthDynamicEqIntegrationTests")
target_sources(RackSynthDynamicEqIntegrationTests PRIVATE tests/native/DynamicEqIntegrationTests.cpp)
target_compile_definitions(RackSynthDynamicEqIntegrationTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/dynamic-eq-legacy")
target_link_libraries(RackSynthDynamicEqIntegrationTests PRIVATE RackSynthProject)
racksynth_add_test(NAME RackSynthDynamicEqIntegrationTests COMMAND RackSynthDynamicEqIntegrationTests)
racksynth_add_test(NAME RackSynthDynamicEqRoutingTests COMMAND RackSynthDynamicEqIntegrationTests --audio)
racksynth_add_test(NAME RackSynthSpectralEqRoutingTests COMMAND RackSynthDynamicEqIntegrationTests --spectral)

juce_add_console_app(RackSynthDynamicEqProcessorTests PRODUCT_NAME "RackSynthDynamicEqProcessorTests")
target_sources(RackSynthDynamicEqProcessorTests PRIVATE tests/native/DynamicEqProcessorTests.cpp)
target_link_libraries(RackSynthDynamicEqProcessorTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthDynamicEqProcessorTests COMMAND RackSynthDynamicEqProcessorTests)

juce_add_console_app(RackSynthDynamicEqRackTests PRODUCT_NAME "RackSynthDynamicEqRackTests")
target_sources(RackSynthDynamicEqRackTests PRIVATE tests/native/DynamicEqRackTests.cpp)
target_compile_definitions(RackSynthDynamicEqRackTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/dynamic-eq-legacy")
target_link_libraries(RackSynthDynamicEqRackTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthDynamicEqRackTests COMMAND RackSynthDynamicEqRackTests)

juce_add_console_app(RackSynthDelayTapeTests PRODUCT_NAME "RackSynthDelayTapeTests")
target_sources(RackSynthDelayTapeTests PRIVATE tests/native/DelayTapeTests.cpp)
target_compile_definitions(RackSynthDelayTapeTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/delay-tape-legacy")
target_link_libraries(RackSynthDelayTapeTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthDelayTapeTests COMMAND RackSynthDelayTapeTests)
juce_add_console_app(RackSynthDelayStereoTests PRODUCT_NAME "RackSynthDelayStereoTests")
target_sources(RackSynthDelayStereoTests PRIVATE tests/native/DelayStereoTests.cpp)
target_compile_definitions(RackSynthDelayStereoTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/delay-stereo-legacy")
target_link_libraries(RackSynthDelayStereoTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthDelayStereoTests COMMAND RackSynthDelayStereoTests)
juce_add_console_app(RackSynthWavetableWarmthTests PRODUCT_NAME "RackSynthWavetableWarmthTests")
target_sources(RackSynthWavetableWarmthTests PRIVATE tests/native/WavetableWarmthTests.cpp)
target_compile_definitions(RackSynthWavetableWarmthTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/wavetable-warmth-legacy")
target_link_libraries(RackSynthWavetableWarmthTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
racksynth_add_test(NAME RackSynthWavetableWarmthTests COMMAND RackSynthWavetableWarmthTests)
juce_add_console_app(RackSynthWavetableWarmthDspTests PRODUCT_NAME "RackSynthWavetableWarmthDspTests")
target_sources(RackSynthWavetableWarmthDspTests PRIVATE tests/native/WavetableWarmthDspTests.cpp)
target_link_libraries(RackSynthWavetableWarmthDspTests PRIVATE RackSynthCore)
racksynth_add_test(NAME RackSynthWavetableWarmthDspTests COMMAND RackSynthWavetableWarmthDspTests)

# CI shares its PC with other work, so a test's timeout catches a hang, not a slow run.
target_sources(RackSynthInstrument PRIVATE src/studio/DynamicEqGraph.cpp src/studio/EqOverview.cpp src/studio/EqMatchPanel.cpp)
target_sources(RackSynthInstrument PRIVATE src/studio/SaturationGraph.cpp)
juce_add_console_app(RackSynthSaturationGraphTests PRODUCT_NAME "RackSynthSaturationGraphTests")
target_sources(RackSynthSaturationGraphTests PRIVATE tests/native/SaturationGraphTests.cpp)
target_link_libraries(RackSynthSaturationGraphTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
if(TARGET RackSynthRackPackageFixtures)
    add_dependencies(RackSynthSaturationGraphTests RackSynthRackPackageFixtures)
endif()
racksynth_add_test(NAME RackSynthSaturationGraphTests COMMAND RackSynthSaturationGraphTests)
racksynth_add_test(NAME RackSynthSaturationGraphTests_offscreen_release COMMAND RackSynthSaturationGraphTests --offscreen-release)
target_link_libraries(RackSynthInstrument PRIVATE RackSynthFxSpectrum)
juce_add_console_app(RackSynthDynamicEqGraphTests PRODUCT_NAME "RackSynthDynamicEqGraphTests")
target_sources(RackSynthDynamicEqGraphTests PRIVATE tests/native/DynamicEqGraphTests.cpp)
target_link_libraries(RackSynthDynamicEqGraphTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer)
racksynth_add_test(NAME RackSynthDynamicEqGraphTests COMMAND RackSynthDynamicEqGraphTests)
juce_add_console_app(RackSynthDynamicEqProductTests PRODUCT_NAME "RackSynthDynamicEqProductTests")
target_sources(RackSynthDynamicEqProductTests PRIVATE tests/native/DynamicEqProductTests.cpp)
target_link_libraries(RackSynthDynamicEqProductTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer)
racksynth_add_test(NAME RackSynthDynamicEqProductTests COMMAND RackSynthDynamicEqProductTests)
if(RACKSYNTH_BUILD_PROBES)
    # Isolated generated Candidate; never linked into a product or default build/test.
    set(saturation_lifecycle_dir "${CMAKE_BINARY_DIR}/saturation-lifecycle-rate")
    add_custom_command(OUTPUT "${saturation_lifecycle_dir}/SaturationCandidate.h"
            "${saturation_lifecycle_dir}/SaturationCandidate.cpp" "${saturation_lifecycle_dir}/source.json"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_lifecycle_rate.py"
            --output "${saturation_lifecycle_dir}"
        DEPENDS tools/saturation_lifecycle_rate.py probes/saturation/SaturationCandidate.h
            probes/saturation/SaturationCandidate.cpp VERBATIM)
    juce_add_console_app(RackSynthSaturationLifecycleRateTests PRODUCT_NAME "RackSynthSaturationLifecycleRateTests")
    set_target_properties(RackSynthSaturationLifecycleRateTests PROPERTIES
        EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    if(TARGET RackSynthSaturationLifecycleRateTests_rc_lib)
        set_target_properties(RackSynthSaturationLifecycleRateTests_rc_lib PROPERTIES
            EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    endif()
    target_sources(RackSynthSaturationLifecycleRateTests PRIVATE
        probes/saturation/lifecycle/SaturationLifecycleRateTests.cpp
        "${saturation_lifecycle_dir}/SaturationCandidate.cpp" "${saturation_lifecycle_dir}/SaturationCandidate.h"
        probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationLifecycleRateTests BEFORE PRIVATE "${saturation_lifecycle_dir}" probes/saturation)
    target_compile_definitions(RackSynthSaturationLifecycleRateTests PRIVATE AIRACK_SATURATION_TRANSITIONS=1
        RACKSYNTH_LIFECYCLE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261023-stable-v2"
        RACKSYNTH_LIFECYCLE_DESCRIPTOR="${saturation_lifecycle_dir}/source.json"
        RACKSYNTH_LIFECYCLE_RESULT_DIR="${CMAKE_BINARY_DIR}/saturation-lifecycle-rate-results/$<CONFIG>")
    target_link_libraries(RackSynthSaturationLifecycleRateTests PRIVATE RackSynthJuce RackSynthSaturationGainRuntime)
    if(WIN32)
        add_dependencies(RackSynthSaturationLifecycleRateTests RackSynthNativeRun)
    endif()
    # Private arithmetic capture only; never part of ALL, CTest or a product image.
    juce_add_console_app(RackSynthSaturationProductStateTests PRODUCT_NAME "RackSynthSaturationProductStateTests")
    set_target_properties(RackSynthSaturationProductStateTests PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(RackSynthSaturationProductStateTests PRIVATE probes/saturation/SaturationProductStateTests.cpp)
    target_include_directories(RackSynthSaturationProductStateTests PRIVATE probes/saturation)
    target_link_libraries(RackSynthSaturationProductStateTests PRIVATE RackSynthJuce)
    add_library(RackSynthSaturationCandidate STATIC probes/saturation/SaturationCandidate.cpp
        probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationCandidate PUBLIC probes/saturation)
    target_link_libraries(RackSynthSaturationCandidate PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
    # Unadopted transition feasibility; never link both candidate variants into one image.
    add_library(RackSynthSaturationTransitionCandidate STATIC probes/saturation/SaturationCandidate.cpp
        probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationTransitionCandidate PUBLIC probes/saturation)
    target_compile_definitions(RackSynthSaturationTransitionCandidate PRIVATE AIRACK_SATURATION_TRANSITIONS=1)
    target_link_libraries(RackSynthSaturationTransitionCandidate PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
    # Optional private same-flags witness; no product or default-build dependency.
    set(RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE "" CACHE PATH "Exact 6d42f0dd private saturation source snapshot")
    if(RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE)
        file(SHA256 "${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/SaturationCandidate.cpp" saturation_optimization_baseline_sha)
        if(NOT saturation_optimization_baseline_sha STREQUAL "1a82b828b19894a693f2bb86c4b0c8e763da757d2cb349351f475c8f009c0ebc")
            message(FATAL_ERROR "Optimization witness requires exact 6d42f0dd baseline source")
        endif()
        add_library(RackSynthSaturationOptimizationBaseline STATIC EXCLUDE_FROM_ALL
            "${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/SaturationCandidate.cpp"
            "${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/SaturationWideNetwork.cpp")
        target_compile_definitions(RackSynthSaturationOptimizationBaseline PRIVATE airack=airack_baseline AIRACK_SATURATION_TRANSITIONS=1)
        target_link_libraries(RackSynthSaturationOptimizationBaseline PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
        add_library(RackSynthSaturationOptimizationCost STATIC EXCLUDE_FROM_ALL
            probes/saturation/SaturationCandidate.cpp probes/saturation/SaturationWideNetwork.cpp)
        target_compile_definitions(RackSynthSaturationOptimizationCost PRIVATE airack=airack_cost AIRACK_SATURATION_TRANSITIONS=1 AIRACK_SATURATION_OPTIMIZATION_TEST=1)
        target_link_libraries(RackSynthSaturationOptimizationCost PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
        juce_add_console_app(RackSynthSaturationOptimizationTests PRODUCT_NAME "RackSynthSaturationOptimizationTests")
        set_target_properties(RackSynthSaturationOptimizationTests PROPERTIES EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        target_sources(RackSynthSaturationOptimizationTests PRIVATE probes/saturation/contracts/SaturationOptimizationTests.cpp)
        configure_file(probes/saturation/SaturationCandidate.h
            "${CMAKE_CURRENT_BINARY_DIR}/saturation-optimization-cost/SaturationCandidate.h" COPYONLY)
        target_compile_definitions(RackSynthSaturationOptimizationTests PRIVATE
            RACKSYNTH_OPTIMIZATION_BASELINE_HEADER="${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/SaturationCandidate.h"
            RACKSYNTH_OPTIMIZATION_COST_HEADER="${CMAKE_CURRENT_BINARY_DIR}/saturation-optimization-cost/SaturationCandidate.h"
            RACKSYNTH_OPTIMIZATION_WORKLOAD_HEADER="${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/../workload.h"
            RACKSYNTH_OPTIMIZATION_ALLOCATION_HEADER="${RACKSYNTH_SATURATION_OPTIMIZATION_BASELINE}/../allocation.h")
        target_link_libraries(RackSynthSaturationOptimizationTests PRIVATE RackSynthSaturationTransitionCandidate
            RackSynthSaturationOptimizationBaseline RackSynthSaturationOptimizationCost)
    endif()
    # Private numerical witness: current source only, never a frozen processor.
    add_library(RackSynthSaturationNumericalCandidate STATIC probes/saturation/SaturationCandidate.cpp
        probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationNumericalCandidate PUBLIC probes/saturation)
    target_compile_definitions(RackSynthSaturationNumericalCandidate PRIVATE AIRACK_SATURATION_TRANSITIONS=1 AIRACK_SATURATION_NUMERICAL_TEST=1)
    target_link_libraries(RackSynthSaturationNumericalCandidate PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
    juce_add_console_app(RackSynthSaturationNumericalTests PRODUCT_NAME "RackSynthSaturationNumericalTests")
    juce_add_console_app(RackSynthSaturationNumericalOriginalTests PRODUCT_NAME "RackSynthSaturationNumericalOriginalTests")
    set(saturation_q_fixtures "${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/numerical-stability-20261022")
    file(SHA256 "${CMAKE_SOURCE_DIR}/probes/saturation/SaturationCandidate.cpp" saturation_q_source_sha)
    file(SHA256 "${saturation_q_fixtures}/q-fixtures.json" saturation_q_fixture_sha)
    set(saturation_q_selectors "${saturation_q_fixtures}/selector-boundaries/q-selector-fixtures.json")
    file(SHA256 "${saturation_q_selectors}" saturation_q_selector_sha)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS probes/saturation/SaturationCandidate.cpp
        "${saturation_q_fixtures}/q-fixtures.json" "${saturation_q_selectors}")
    foreach(numerical_target RackSynthSaturationNumericalTests RackSynthSaturationNumericalOriginalTests)
        target_sources(${numerical_target} PRIVATE probes/saturation/SaturationNumericalTests.cpp)
        target_include_directories(${numerical_target} PRIVATE "${saturation_q_fixtures}")
        target_compile_definitions(${numerical_target} PRIVATE AIRACK_SATURATION_NUMERICAL_TEST=1
            RACKSYNTH_Q_FIXTURES="${saturation_q_fixtures}/q-fixtures.json" RACKSYNTH_Q_SOURCE_SHA="${saturation_q_source_sha}"
            RACKSYNTH_Q_FILE_SHA="${saturation_q_fixture_sha}" RACKSYNTH_Q_SELECTOR_FIXTURES="${saturation_q_selectors}"
            RACKSYNTH_Q_SELECTOR_SHA="${saturation_q_selector_sha}")
        target_link_libraries(${numerical_target} PRIVATE RackSynthSaturationNumericalCandidate)
    endforeach()
    # Unregistered sensitivity control; it must fail the real MP comparisons.
    target_compile_definitions(RackSynthSaturationNumericalOriginalTests PRIVATE AIRACK_SATURATION_NUMERICAL_ORIGINAL_EXPRESSION=1)
    racksynth_add_test(NAME RackSynthSaturationNumericalTests COMMAND RackSynthSaturationNumericalTests)
    racksynth_add_test(NAME RackSynthSaturationNumericalPython COMMAND "${Python3_EXECUTABLE}"
        -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tests" -p test_saturation_numerical_stability.py -v)
    racksynth_add_test(NAME RackSynthSaturationFrontRecurrencePython COMMAND "${Python3_EXECUTABLE}"
        -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tests" -p test_saturation_front_recurrence.py -v)
    juce_add_console_app(RackSynthSaturationTransitionTests PRODUCT_NAME "RackSynthSaturationTransitionTests")
    target_sources(RackSynthSaturationTransitionTests PRIVATE probes/saturation/SaturationTransitionTests.cpp)
    target_compile_definitions(RackSynthSaturationTransitionTests PRIVATE RACKSYNTH_SATURATION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021")
    target_link_libraries(RackSynthSaturationTransitionTests PRIVATE RackSynthSaturationTransitionCandidate)
    racksynth_add_test(NAME RackSynthSaturationTransitionTests COMMAND RackSynthSaturationTransitionTests)
    add_library(RackSynthSaturationControlReferenceCandidate STATIC probes/saturation/SaturationCandidate.cpp
        probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationControlReferenceCandidate PUBLIC probes/saturation)
    target_compile_definitions(RackSynthSaturationControlReferenceCandidate PRIVATE AIRACK_SATURATION_REFERENCE_TRACE=1)
    target_link_libraries(RackSynthSaturationControlReferenceCandidate PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
    juce_add_console_app(RackSynthSaturationControlReference PRODUCT_NAME "RackSynthSaturationControlReference")
    target_sources(RackSynthSaturationControlReference PRIVATE probes/saturation/contracts/SaturationControlLawProbe.cpp)
    target_compile_definitions(RackSynthSaturationControlReference PRIVATE RACKSYNTH_CONTROL_REFERENCE=1)
    target_link_libraries(RackSynthSaturationControlReference PRIVATE RackSynthSaturationControlReferenceCandidate)
    juce_add_console_app(RackSynthSaturationControlActual PRODUCT_NAME "RackSynthSaturationControlActual")
    target_sources(RackSynthSaturationControlActual PRIVATE probes/saturation/contracts/SaturationControlLawProbe.cpp)
    target_link_libraries(RackSynthSaturationControlActual PRIVATE RackSynthSaturationTransitionCandidate)
    juce_add_console_app(RackSynthSaturationHistoryLegacy PRODUCT_NAME "RackSynthSaturationHistoryLegacy")
    target_sources(RackSynthSaturationHistoryLegacy PRIVATE probes/saturation/contracts/SaturationHistoryProbe.cpp)
    target_compile_definitions(RackSynthSaturationHistoryLegacy PRIVATE RACKSYNTH_HISTORY_LEGACY=1)
    target_link_libraries(RackSynthSaturationHistoryLegacy PRIVATE RackSynthSaturationCandidate)
    juce_add_console_app(RackSynthSaturationHistoryActual PRODUCT_NAME "RackSynthSaturationHistoryActual")
    target_sources(RackSynthSaturationHistoryActual PRIVATE probes/saturation/contracts/SaturationHistoryProbe.cpp)
    target_link_libraries(RackSynthSaturationHistoryActual PRIVATE RackSynthSaturationTransitionCandidate)
    juce_add_console_app(RackSynthSaturationHistoryDiagnosis PRODUCT_NAME "RackSynthSaturationHistoryDiagnosis")
    target_sources(RackSynthSaturationHistoryDiagnosis PRIVATE probes/saturation/contracts/SaturationHistoryDiagnosis.cpp)
    target_link_libraries(RackSynthSaturationHistoryDiagnosis PRIVATE RackSynthSaturationTransitionCandidate)
    juce_add_console_app(RackSynthSaturationNumericalReplay PRODUCT_NAME "RackSynthSaturationNumericalReplay")
    target_sources(RackSynthSaturationNumericalReplay PRIVATE probes/saturation/contracts/SaturationHistoryDiagnosis.cpp)
    target_compile_definitions(RackSynthSaturationNumericalReplay PRIVATE RACKSYNTH_HISTORY_RETURN_REPLAY=1)
    target_link_libraries(RackSynthSaturationNumericalReplay PRIVATE RackSynthSaturationTransitionCandidate)
    racksynth_add_test(NAME RackSynthSaturationNumericalReplaySelfTest COMMAND RackSynthSaturationNumericalReplay --self-test)
    set(saturation_descriptor "${CMAKE_BINARY_DIR}/saturation-numerical-stability/$<CONFIG>/current-source.json")
    add_custom_target(RackSynthSaturationCurrentSourceDescriptor
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_numerical_stability.py"
            --bind-source "${saturation_descriptor}" --variant current-precision64-full-audio-no-cells --build "${CMAKE_BINARY_DIR}"
            --working-build-descriptor --configuration "$<CONFIG>"
        BYPRODUCTS "${saturation_descriptor}"
        DEPENDS RackSynthSaturationTransitionCandidate VERBATIM)
    set(numerical_trace_candidate "${CMAKE_BINARY_DIR}/saturation-numerical-stability/$<CONFIG>/SaturationCandidateObserved.cpp")
    add_custom_command(OUTPUT "${numerical_trace_candidate}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_numerical_stability.py"
            --generate-observer "${numerical_trace_candidate}" --source-descriptor "${saturation_descriptor}"
            --variant current-precision64-full-audio-no-cells
            --build "${CMAKE_BINARY_DIR}" --configuration "$<CONFIG>" --working-build-observer
        DEPENDS probes/saturation/SaturationCandidate.cpp probes/saturation/SaturationCandidate.h
            probes/saturation/contracts/SaturationNumericalWitness.h probes/saturation/SaturationProductState.h tools/saturation_numerical_stability.py
            tools/saturation_history_trace.py VERBATIM)
    add_library(RackSynthSaturationNumericalTraceCandidate STATIC "${numerical_trace_candidate}" probes/saturation/SaturationWideNetwork.cpp)
    add_dependencies(RackSynthSaturationNumericalTraceCandidate RackSynthSaturationCurrentSourceDescriptor)
    target_include_directories(RackSynthSaturationNumericalTraceCandidate PUBLIC probes/saturation PRIVATE probes/saturation/contracts)
    target_compile_definitions(RackSynthSaturationNumericalTraceCandidate PRIVATE AIRACK_SATURATION_TRANSITIONS=1 AIRACK_SATURATION_REFERENCE_TRACE=1)
    target_link_libraries(RackSynthSaturationNumericalTraceCandidate PUBLIC RackSynthJuce RackSynthSaturationGainRuntime)
    juce_add_console_app(RackSynthSaturationNumericalTrace PRODUCT_NAME "RackSynthSaturationNumericalTrace")
    target_sources(RackSynthSaturationNumericalTrace PRIVATE probes/saturation/contracts/SaturationHistoryDiagnosis.cpp
        probes/saturation/contracts/SaturationHistoryTrace.cpp)
    target_compile_definitions(RackSynthSaturationNumericalTrace PRIVATE RACKSYNTH_HISTORY_TRACE=1)
    target_link_libraries(RackSynthSaturationNumericalTrace PRIVATE RackSynthSaturationNumericalTraceCandidate)
    foreach(target RackSynthSaturationControlReference RackSynthSaturationControlActual
                   RackSynthSaturationHistoryLegacy RackSynthSaturationHistoryActual
                   RackSynthSaturationHistoryDiagnosis RackSynthSaturationNumericalReplay)
        add_dependencies(${target} RackSynthSaturationCurrentSourceDescriptor)
    endforeach()
    set(saturation_baseline "${CMAKE_SOURCE_DIR}/probes/saturation/contracts/baseline-8b7-20261022")
    set(saturation_baseline_dependencies "${saturation_baseline}/manifest.json")
    foreach(name SaturationCandidate.cpp SaturationCandidate.h SaturationWideNetwork.cpp
                 SaturationWideNetwork.h SaturationMath.h SaturationGain.h SaturationCalibration.h)
        list(APPEND saturation_baseline_dependencies "${saturation_baseline}/${name}")
    endforeach()
    file(SHA256 "${saturation_baseline}/manifest.json" saturation_baseline_manifest_sha)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${saturation_baseline}/manifest.json")
    set(saturation_prepared "${CMAKE_BINARY_DIR}/saturation-frozen-baseline-8b7-20261022-${saturation_baseline_manifest_sha}")
    set(saturation_prepared_outputs)
    foreach(source IN LISTS saturation_baseline_dependencies)
        get_filename_component(name "${source}" NAME)
        list(APPEND saturation_prepared_outputs "${saturation_prepared}/${name}")
    endforeach()
    add_custom_command(OUTPUT ${saturation_prepared_outputs}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_frozen_source.py"
            --source "${saturation_baseline}" --output "${saturation_prepared}" --cmake-output
        DEPENDS ${saturation_baseline_dependencies} tools/saturation_frozen_source.py
            tools/saturation_numerical_stability.py
            docs/verification/saturation-numerical-stability-20261022/starting-source.json VERBATIM)
    add_custom_target(RackSynthSaturationFrozenSource DEPENDS ${saturation_prepared_outputs})
    set(history_trace_candidate "${CMAKE_BINARY_DIR}/saturation-history-trace/SaturationCandidateObserved.cpp")
    add_custom_command(OUTPUT "${history_trace_candidate}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_history_trace.py"
            --generate "${history_trace_candidate}" --source "${saturation_prepared}/SaturationCandidate.cpp"
        DEPENDS ${saturation_prepared_outputs} tools/saturation_history_trace.py
            tools/saturation_numerical_stability.py VERBATIM)
    add_library(RackSynthSaturationHistoryTraceCandidate STATIC "${history_trace_candidate}"
        "${saturation_baseline}/SaturationWideNetwork.cpp")
    add_dependencies(RackSynthSaturationHistoryTraceCandidate RackSynthSaturationFrozenSource)
    target_include_directories(RackSynthSaturationHistoryTraceCandidate PUBLIC probes/saturation
        PRIVATE probes/saturation/contracts)
    target_compile_definitions(RackSynthSaturationHistoryTraceCandidate PRIVATE
        AIRACK_SATURATION_TRANSITIONS=1 AIRACK_SATURATION_REFERENCE_TRACE=1)
    target_link_libraries(RackSynthSaturationHistoryTraceCandidate PUBLIC RackSynthJuce)
    juce_add_console_app(RackSynthSaturationHistoryTrace PRODUCT_NAME "RackSynthSaturationHistoryTrace")
    target_sources(RackSynthSaturationHistoryTrace PRIVATE probes/saturation/contracts/SaturationHistoryDiagnosis.cpp
        probes/saturation/contracts/SaturationHistoryTrace.cpp)
    target_compile_definitions(RackSynthSaturationHistoryTrace PRIVATE RACKSYNTH_HISTORY_TRACE=1)
    target_link_libraries(RackSynthSaturationHistoryTrace PRIVATE RackSynthSaturationHistoryTraceCandidate)
    target_include_directories(RackSynthSaturationHistoryTraceCandidate BEFORE PRIVATE "${saturation_baseline}")
    target_include_directories(RackSynthSaturationHistoryTrace BEFORE PRIVATE "${saturation_baseline}")
    racksynth_add_test(NAME RackSynthSaturationHistoryTraceSelfTest COMMAND RackSynthSaturationHistoryTrace --self-test)
    racksynth_add_test(NAME RackSynthSaturationHistoryTracePython COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_saturation_history_trace.py")
    # Coefficient-only prepare export: no processing and no change to retained observers.
    set(saturation_decay_export "${CMAKE_BINARY_DIR}/saturation-decay-bounds/SaturationCoefficientExport.cpp")
    add_custom_command(OUTPUT "${saturation_decay_export}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_decay_bounds.py"
            --generate "${saturation_decay_export}" --source "${saturation_prepared}/SaturationCandidate.cpp"
        DEPENDS ${saturation_prepared_outputs} tools/saturation_decay_bounds.py tools/saturation_numerical_stability.py
            docs/verification/saturation-history-diagnosis-20261021/task2-stage-diagnosis.json VERBATIM)
    juce_add_console_app(RackSynthSaturationCoefficientExport PRODUCT_NAME "RackSynthSaturationCoefficientExport")
    add_dependencies(RackSynthSaturationCoefficientExport RackSynthSaturationFrozenSource)
    target_sources(RackSynthSaturationCoefficientExport PRIVATE "${saturation_decay_export}"
        "${saturation_baseline}/SaturationWideNetwork.cpp")
    target_include_directories(RackSynthSaturationCoefficientExport PRIVATE probes/saturation probes/saturation/contracts)
    target_include_directories(RackSynthSaturationCoefficientExport BEFORE PRIVATE "${saturation_baseline}")
    foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS COMPILE_FLAGS)
        get_source_file_property(baseline_property probes/saturation/SaturationWideNetwork.cpp ${property})
        if(NOT baseline_property STREQUAL "NOTFOUND")
            set_source_files_properties("${saturation_baseline}/SaturationWideNetwork.cpp" PROPERTIES ${property} "${baseline_property}")
        endif()
    endforeach()
    target_compile_definitions(RackSynthSaturationCoefficientExport PRIVATE AIRACK_SATURATION_TRANSITIONS=1)
    target_link_libraries(RackSynthSaturationCoefficientExport PRIVATE RackSynthJuce)
    # These unlinked diagnostics retain the serial compiler policy bound by the frozen readers.
    foreach(target IN ITEMS RackSynthSaturationTransitionCandidate
                            RackSynthSaturationNumericalTraceCandidate
                            RackSynthSaturationHistoryTraceCandidate
                            RackSynthSaturationCoefficientExport)
        set_property(TARGET ${target} PROPERTY RACKSYNTH_SATURATION_FROZEN_COMPILER_POLICY TRUE)
    endforeach()
    racksynth_add_test(NAME RackSynthSaturationDecayBoundsPython COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_saturation_decay_bounds.py")
    racksynth_add_test(NAME RackSynthSaturationHistoryDiagnosisSelfTest COMMAND RackSynthSaturationHistoryDiagnosis --self-test)
    racksynth_add_test(NAME RackSynthSaturationHistoryDiagnosisPython COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_saturation_history_diagnosis.py" --self-test)
    foreach(factor 4 64)
        foreach(rate 44100 48000 96000)
            foreach(family modes cuts)
                if(factor EQUAL 64 AND family STREQUAL "modes")
                    foreach(character RANGE 0 2)
                        racksynth_add_test(NAME RackSynthSaturationHistory_${factor}_${rate}_${family}_${character}
                            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_saturation_histories.py"
                            --legacy $<TARGET_FILE:RackSynthSaturationHistoryLegacy>
                            --actual $<TARGET_FILE:RackSynthSaturationHistoryActual>
                            --factor ${factor} --rate ${rate} --family ${family} --character ${character}
                            --source-descriptor "${saturation_descriptor}")
                    endforeach()
                else()
                    racksynth_add_test(NAME RackSynthSaturationHistory_${factor}_${rate}_${family}
                        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_saturation_histories.py"
                        --legacy $<TARGET_FILE:RackSynthSaturationHistoryLegacy>
                        --actual $<TARGET_FILE:RackSynthSaturationHistoryActual>
                        --factor ${factor} --rate ${rate} --family ${family} --source-descriptor "${saturation_descriptor}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    foreach(factor 4 64)
        foreach(rate 44100 48000 96000)
            racksynth_add_test(NAME RackSynthSaturationControlLaw_${factor}_${rate}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_saturation_control_laws.py"
                --reference $<TARGET_FILE:RackSynthSaturationControlReference>
                --actual $<TARGET_FILE:RackSynthSaturationControlActual> --factor ${factor} --rate ${rate}
                --source-descriptor "${saturation_descriptor}")
        endforeach()
    endforeach()
    juce_add_console_app(RackSynthSaturationChargedReference PRODUCT_NAME "RackSynthSaturationChargedReference")
    target_sources(RackSynthSaturationChargedReference PRIVATE probes/saturation/contracts/SaturationChargedFadeProbe.cpp)
    target_compile_definitions(RackSynthSaturationChargedReference PRIVATE RACKSYNTH_CHARGED_REFERENCE=1)
    target_link_libraries(RackSynthSaturationChargedReference PRIVATE RackSynthSaturationCandidate)
    juce_add_console_app(RackSynthSaturationChargedActual PRODUCT_NAME "RackSynthSaturationChargedActual")
    target_sources(RackSynthSaturationChargedActual PRIVATE probes/saturation/contracts/SaturationChargedFadeProbe.cpp)
    target_link_libraries(RackSynthSaturationChargedActual PRIVATE RackSynthSaturationTransitionCandidate)
    foreach(factor IN ITEMS 4 64)
        foreach(rate IN ITEMS 44100 48000 96000)
            racksynth_add_test(NAME RackSynthSaturationChargedFade_${factor}_${rate}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_saturation_charged_fades.py"
                --reference $<TARGET_FILE:RackSynthSaturationChargedReference>
                --actual $<TARGET_FILE:RackSynthSaturationChargedActual> --factor ${factor} --rate ${rate})
        endforeach()
    endforeach()
    add_library(RackSynthSaturationAnalysis STATIC probes/saturation/SaturationAnalysis.cpp)
    target_include_directories(RackSynthSaturationAnalysis PUBLIC probes/saturation)
    target_link_libraries(RackSynthSaturationAnalysis PUBLIC RackSynthJuce)
    add_library(RackSynthSaturationFactoryReference STATIC
        probes/saturation/SaturationCandidate.cpp probes/saturation/SaturationWideNetwork.cpp)
    target_compile_definitions(RackSynthSaturationFactoryReference PRIVATE
        airack=airack_reference AIRACK_SATURATION_TRANSITIONS=1)
    target_include_directories(RackSynthSaturationFactoryReference PUBLIC probes/saturation)
    target_link_libraries(RackSynthSaturationFactoryReference PUBLIC
        RackSynthJuce RackSynthSaturationGainRuntime)
    # Replace FactoryCompare's TransitionCandidate link only, retaining Analysis and State.
    juce_add_console_app(RackSynthSaturationFactoryCompare PRODUCT_NAME "RackSynthSaturationFactoryCompare")
    target_sources(RackSynthSaturationFactoryCompare PRIVATE probes/saturation/SaturationFactoryCompare.cpp)
    target_include_directories(RackSynthSaturationFactoryCompare PRIVATE src/racks)
    target_link_libraries(RackSynthSaturationFactoryCompare PRIVATE RackSynthSaturationFactoryReference RackSynthSaturationAnalysis RackSynthState RackSynthSaturation)
    juce_add_console_app(RackSynthSaturationProbeTests PRODUCT_NAME "RackSynthSaturationProbeTests")
    target_sources(RackSynthSaturationProbeTests PRIVATE probes/saturation/SaturationProbeTests.cpp)
    target_include_directories(RackSynthSaturationProbeTests PRIVATE probes/saturation)
    target_link_libraries(RackSynthSaturationProbeTests PRIVATE RackSynthSaturationCandidate)
    racksynth_add_test(NAME RackSynthSaturationProbeTests COMMAND RackSynthSaturationProbeTests)

    juce_add_console_app(RackSynthSaturationContractTests PRODUCT_NAME "RackSynthSaturationContractTests")
    target_sources(RackSynthSaturationContractTests PRIVATE probes/saturation/contracts/SaturationContractTests.cpp)
    target_link_libraries(RackSynthSaturationContractTests PRIVATE RackSynthSaturationCandidate)
    racksynth_add_test(NAME RackSynthSaturationContractTests COMMAND RackSynthSaturationContractTests)
    racksynth_add_test(NAME RackSynthSaturationContractTests_negative_control COMMAND RackSynthSaturationContractTests --negative-control)

    juce_add_console_app(RackSynthSaturationTransitionContractTests PRODUCT_NAME "RackSynthSaturationTransitionContractTests")
    target_sources(RackSynthSaturationTransitionContractTests PRIVATE probes/saturation/contracts/SaturationContractTests.cpp)
    target_compile_definitions(RackSynthSaturationTransitionContractTests PRIVATE AIRACK_SATURATION_TRANSITION_CONTRACTS=1)
    target_link_libraries(RackSynthSaturationTransitionContractTests PRIVATE RackSynthSaturationTransitionCandidate)
    racksynth_add_test(NAME RackSynthSaturationTransitionContractTests COMMAND RackSynthSaturationTransitionContractTests)
    racksynth_add_test(NAME RackSynthSaturationTransitionContractTests_negative_control COMMAND RackSynthSaturationTransitionContractTests --negative-control)

    juce_add_console_app(RackSynthSaturationFixtureTests PRODUCT_NAME "RackSynthSaturationFixtureTests")
    target_sources(RackSynthSaturationFixtureTests PRIVATE probes/saturation/contracts/SaturationFixtureTests.cpp)
    target_link_libraries(RackSynthSaturationFixtureTests PRIVATE RackSynthSaturationCandidate)
    target_compile_definitions(RackSynthSaturationFixtureTests PRIVATE RACKSYNTH_SATURATION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021")

    juce_add_console_app(RackSynthSaturationFixtureDiagnosis PRODUCT_NAME "RackSynthSaturationFixtureDiagnosis")
    target_sources(RackSynthSaturationFixtureDiagnosis PRIVATE probes/saturation/contracts/SaturationFixtureTests.cpp)
    target_link_libraries(RackSynthSaturationFixtureDiagnosis PRIVATE RackSynthSaturationCandidate)
    target_compile_definitions(RackSynthSaturationFixtureDiagnosis PRIVATE RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS=1
        RACKSYNTH_SATURATION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021")

    # The predecessor consumer remains directly runnable; v2 is the explicitly amended acceptance target.
    add_custom_target(RackSynthSaturationFixtureRevisionTests
        DEPENDS RackSynthSaturationFixtureDiagnosis)
    if(WIN32)
        add_dependencies(RackSynthSaturationFixtureRevisionTests RackSynthNativeRun)
    endif()
    racksynth_add_test(NAME RackSynthSaturationFixtureRevisionTests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_fixture_revision.py"
            --native "$<TARGET_FILE:RackSynthSaturationFixtureDiagnosis>"
            --output-root "${CMAKE_BINARY_DIR}/saturation-fixture-revision/$<CONFIG>")

    set(saturation_serial_observer "${CMAKE_BINARY_DIR}/saturation-serial-observer")
    set(saturation_serial_candidate "${saturation_serial_observer}/SaturationCandidateObserved.cpp")
    set(saturation_serial_driver "${saturation_serial_observer}/SaturationFixtureObserved.cpp")
    file(GLOB saturation_serial_inputs CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/probes/saturation/*.cpp" "${CMAKE_SOURCE_DIR}/probes/saturation/*.h"
        "${CMAKE_SOURCE_DIR}/probes/saturation/contracts/*.h"
        "${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021/*"
        "${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261023-stable-v2/*")
    add_custom_command(OUTPUT "${saturation_serial_candidate}" "${saturation_serial_driver}"
        "${saturation_serial_observer}/source.json" "${saturation_serial_observer}/SaturationSerialSource.h"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_serial_observer.py"
            --generate "${saturation_serial_observer}"
        DEPENDS ${saturation_serial_inputs} probes/saturation/contracts/SaturationFixtureTests.cpp
            tools/saturation_serial_observer.py tools/saturation_fixture_revision.py VERBATIM)
    juce_add_console_app(RackSynthSaturationSerialObserver PRODUCT_NAME "RackSynthSaturationSerialObserver")
    set_target_properties(RackSynthSaturationSerialObserver PROPERTIES
        EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    target_sources(RackSynthSaturationSerialObserver PRIVATE "${saturation_serial_candidate}"
        "${saturation_serial_driver}" probes/saturation/SaturationWideNetwork.cpp)
    target_include_directories(RackSynthSaturationSerialObserver PRIVATE probes/saturation probes/saturation/contracts)
    target_compile_definitions(RackSynthSaturationSerialObserver PRIVATE
        RACKSYNTH_SATURATION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021"
        RACKSYNTH_SERIAL_SOURCE_DESCRIPTOR="${saturation_serial_observer}/source.json")
    target_link_libraries(RackSynthSaturationSerialObserver PRIVATE RackSynthJuce RackSynthSaturationGainRuntime)
    # End normal serial observer.

    # Private canonical gain runtime; scalar feasibility targets remain excluded.
    add_library(RackSynthSaturationGainRuntime INTERFACE)
    target_include_directories(RackSynthSaturationGainRuntime INTERFACE probes/saturation)
    if(WIN32)
        if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
           OR NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
            message(FATAL_ERROR "Saturation private gain import requires validated MSVC Windows x64")
        endif()
        enable_language(ASM_MASM)
        add_library(RackSynthSaturationGainImport STATIC probes/saturation/SaturationGainImport.asm)
        set_target_properties(RackSynthSaturationGainImport PROPERTIES EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        target_link_libraries(RackSynthSaturationGainRuntime INTERFACE RackSynthSaturationGainImport)
        set(saturation_gain_source "${CMAKE_BINARY_DIR}/saturation-gain-source-coff/$<CONFIG>")
        set(saturation_gain_object "${CMAKE_BINARY_DIR}/RackSynthSaturationGainImport.dir/$<CONFIG>/SaturationGainImport.obj")
        add_custom_command(OUTPUT "${saturation_gain_source}/source.json" "${saturation_gain_source}/GainParitySource.h"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_gain_parity.py"
                --prepare "${saturation_gain_source}" --archive "$<TARGET_FILE:RackSynthSaturationGainImport>"
                --object "${saturation_gain_object}"
            DEPENDS RackSynthSaturationGainImport CMakeLists.txt tools/saturation_gain_import.py
                tools/saturation_gain_parity.py probes/saturation/SaturationGainImport.asm
                probes/saturation/SaturationGain.h probes/saturation/SaturationGainRuntime.h
                probes/saturation/contracts/SaturationGainParity.cpp probes/saturation/contracts/SaturationGainLinkTests.cpp VERBATIM)
        add_executable(RackSynthSaturationGainLinkTests EXCLUDE_FROM_ALL probes/saturation/contracts/SaturationGainLinkTests.cpp)
        target_link_libraries(RackSynthSaturationGainLinkTests PRIVATE RackSynthSaturationGainRuntime)
        juce_add_console_app(RackSynthSaturationGainParity PRODUCT_NAME "RackSynthSaturationGainParity")
        target_sources(RackSynthSaturationGainParity PRIVATE probes/saturation/contracts/SaturationGainParity.cpp
            "${saturation_gain_source}/GainParitySource.h")
        target_include_directories(RackSynthSaturationGainParity PRIVATE "${saturation_gain_source}")
        target_link_libraries(RackSynthSaturationGainParity PRIVATE RackSynthJuce RackSynthSaturationGainRuntime)
        set_target_properties(RackSynthSaturationGainParity RackSynthSaturationGainLinkTests PROPERTIES
            EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        if(CMAKE_GENERATOR MATCHES "^Visual Studio ")
            set(saturation_producer_props "${CMAKE_BINARY_DIR}/saturation-producer-tracking.props")
            file(CONFIGURE OUTPUT "${saturation_producer_props}" CONTENT [=[
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props"
          Condition="Exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')"
          Label="LocalAppDataPlatform" />
  <Target Name="RackSynthInitializeSaturationProducerTracking"
          BeforeTargets="FixupCLCompileOptions"
          Condition="'$(DesignTimeBuild)' != 'true' and '@(ClCompile)' != ''">
    <!-- Missing/empty tracking requires a real compile before canonical compaction. -->
    <Error Condition="'$(TLogLocation)' == ''" Text="Saturation producer tracking requires TLogLocation." />
    <GetFileHash Files="$(TLogLocation)CL.write.1.tlog" Algorithm="SHA256"
                 Condition="Exists('$(TLogLocation)CL.write.1.tlog')">
      <Output TaskParameter="Hash" PropertyName="RackSynthSaturationProducerWriteHash" />
    </GetFileHash>
    <PropertyGroup>
      <ForceRebuild Condition="!Exists('$(TLogLocation)CL.write.1.tlog') or '$(RackSynthSaturationProducerWriteHash)' == 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855'">true</ForceRebuild>
    </PropertyGroup>
    <MakeDir Directories="$(TLogLocation)" />
    <Touch Files="$(TLogLocation)CL.write.1.tlog" AlwaysCreate="true"
           Condition="!Exists('$(TLogLocation)CL.write.1.tlog')" />
  </Target>
</Project>
]=] @ONLY NEWLINE_STYLE UNIX)
            set_property(TARGET RackSynthSaturationCandidate PROPERTY VS_USER_PROPS "${saturation_producer_props}")
        endif()
        add_custom_target(RackSynthSaturationFixtureProducerProof ALL
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/saturation_gain_parity.py"
                --bind-fixture-producer "$<TARGET_FILE:RackSynthSaturationFixtureDiagnosis>"
            DEPENDS RackSynthSaturationFixtureDiagnosis RackSynthSaturationGainImport VERBATIM)
        add_dependencies(RackSynthSaturationFixtureRevisionTests RackSynthSaturationFixtureProducerProof)
    endif()

    juce_add_console_app(RackSynthSaturationFixtureQuotients PRODUCT_NAME "RackSynthSaturationFixtureQuotients")
    target_sources(RackSynthSaturationFixtureQuotients PRIVATE probes/saturation/contracts/SaturationFixtureTests.cpp)
    target_link_libraries(RackSynthSaturationFixtureQuotients PRIVATE RackSynthSaturationNumericalCandidate)
    target_compile_definitions(RackSynthSaturationFixtureQuotients PRIVATE RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS=1
        AIRACK_SATURATION_NUMERICAL_TEST=1
        RACKSYNTH_SATURATION_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/probes/saturation/contracts/fixtures/candidate-20261021")
    racksynth_add_test(NAME RackSynthSaturationFixtureQuotientsPython COMMAND "${Python3_EXECUTABLE}"
        -B -m unittest discover -s "${CMAKE_SOURCE_DIR}/tests" -p test_saturation_fixture_quotients.py -v)

    juce_add_console_app(RackSynthSaturationCurveTests PRODUCT_NAME "RackSynthSaturationCurveTests")
    target_sources(RackSynthSaturationCurveTests PRIVATE probes/saturation/SaturationCurveTests.cpp)
    target_link_libraries(RackSynthSaturationCurveTests PRIVATE RackSynthSaturationCandidate)
    racksynth_add_test(NAME RackSynthSaturationCurveTests COMMAND RackSynthSaturationCurveTests)

    juce_add_console_app(RackSynthSaturationAnalysisTests PRODUCT_NAME "RackSynthSaturationAnalysisTests")
    target_sources(RackSynthSaturationAnalysisTests PRIVATE probes/saturation/SaturationAnalysisTests.cpp)
    target_include_directories(RackSynthSaturationAnalysisTests PRIVATE probes/saturation)
    target_link_libraries(RackSynthSaturationAnalysisTests PRIVATE RackSynthSaturationAnalysis)
    racksynth_add_test(NAME RackSynthSaturationAnalysisTests COMMAND RackSynthSaturationAnalysisTests)

    juce_add_console_app(RackSynthSaturationFeasibility PRODUCT_NAME "RackSynthSaturationFeasibility")
    target_sources(RackSynthSaturationFeasibility PRIVATE probes/saturation/SaturationFeasibility.cpp)
    target_link_libraries(RackSynthSaturationFeasibility PRIVATE RackSynthSaturationCandidate RackSynthSaturationAnalysis)

    juce_add_console_app(RackSynthSaturationQuality PRODUCT_NAME "RackSynthSaturationQuality")
    target_sources(RackSynthSaturationQuality PRIVATE probes/saturation/SaturationFeasibility.cpp)
    target_compile_definitions(RackSynthSaturationQuality PRIVATE RACKSYNTH_SATURATION_QUALITY_ONLY=1)
    target_link_libraries(RackSynthSaturationQuality PRIVATE RackSynthSaturationCandidate RackSynthSaturationAnalysis)
    juce_add_console_app(RackSynthSaturationTransitionQuality PRODUCT_NAME "RackSynthSaturationTransitionQuality")
    target_sources(RackSynthSaturationTransitionQuality PRIVATE probes/saturation/SaturationFeasibility.cpp)
    target_compile_definitions(RackSynthSaturationTransitionQuality PRIVATE RACKSYNTH_SATURATION_QUALITY_ONLY=1)
    target_link_libraries(RackSynthSaturationTransitionQuality PRIVATE RackSynthSaturationTransitionCandidate RackSynthSaturationAnalysis)

    juce_add_console_app(RackSynthSaturationQualityPolicyTests PRODUCT_NAME "RackSynthSaturationQualityPolicyTests")
    target_sources(RackSynthSaturationQualityPolicyTests PRIVATE probes/saturation/SaturationQualityPolicyTests.cpp)
    target_compile_definitions(RackSynthSaturationQualityPolicyTests PRIVATE RACKSYNTH_SATURATION_QUALITY_ONLY=1)
    target_link_libraries(RackSynthSaturationQualityPolicyTests PRIVATE RackSynthSaturationCandidate RackSynthSaturationAnalysis)

    juce_add_console_app(RackSynthSaturationTransitionCpu PRODUCT_NAME "RackSynthSaturationTransitionCpu")
    target_sources(RackSynthSaturationTransitionCpu PRIVATE probes/saturation/SaturationFeasibility.cpp)
    target_compile_definitions(RackSynthSaturationTransitionCpu PRIVATE RACKSYNTH_SATURATION_CPU_ONLY=1)
    target_link_libraries(RackSynthSaturationTransitionCpu PRIVATE RackSynthSaturationTransitionCandidate RackSynthSaturationAnalysis)
    juce_add_console_app(RackSynthSaturationCpuPolicyTests PRODUCT_NAME "RackSynthSaturationCpuPolicyTests")
    target_sources(RackSynthSaturationCpuPolicyTests PRIVATE probes/saturation/SaturationFeasibility.cpp probes/saturation/SaturationCpuPolicyTests.cpp)
    target_compile_definitions(RackSynthSaturationCpuPolicyTests PRIVATE RACKSYNTH_SATURATION_CPU_ONLY=1 RACKSYNTH_SATURATION_CPU_POLICY=1)
    target_link_libraries(RackSynthSaturationCpuPolicyTests PRIVATE RackSynthSaturationTransitionCandidate RackSynthSaturationAnalysis)
    foreach(check IN ITEMS policy description refusals)
        racksynth_add_test(NAME RackSynthSaturationCpu_${check}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_saturation_cpu_runner.py"
            --cpu $<TARGET_FILE:RackSynthSaturationTransitionCpu> --policy $<TARGET_FILE:RackSynthSaturationCpuPolicyTests>
            --check ${check} --output-root "${CMAKE_BINARY_DIR}/saturation-cpu-preparation/${check}")
    endforeach()

    juce_add_console_app(RackSynthSaturationStress PRODUCT_NAME "RackSynthSaturationStress")
    target_sources(RackSynthSaturationStress PRIVATE probes/saturation/SaturationStress.cpp)
    target_link_libraries(RackSynthSaturationStress PRIVATE RackSynthSaturationCandidate RackSynthSaturationAnalysis)
    racksynth_add_test(NAME RackSynthSaturationStress_self_test COMMAND RackSynthSaturationStress --self-test)
endif()
juce_add_console_app(RackSynthSpaceReverbProductModelTests PRODUCT_NAME "RackSynthSpaceReverbProductModelTests")
target_sources(RackSynthSpaceReverbProductModelTests PRIVATE tests/native/SpaceReverbProductModelTests.cpp)
target_link_libraries(RackSynthSpaceReverbProductModelTests PRIVATE RackSynthProject RackSynthSpaceReverbControls)
target_compile_definitions(RackSynthSpaceReverbProductModelTests PRIVATE RACKSYNTH_SPACE_BASELINE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/space-reverb-legacy")
racksynth_add_test(NAME RackSynthSpaceReverbProductModelTests COMMAND RackSynthSpaceReverbProductModelTests)
juce_add_console_app(RackSynthSpaceReverbRackTests PRODUCT_NAME "RackSynthSpaceReverbRackTests")
target_sources(RackSynthSpaceReverbRackTests PRIVATE tests/native/SpaceReverbRackTests.cpp)
target_link_libraries(RackSynthSpaceReverbRackTests PRIVATE RackSynthCompanionWire RackSynthAiGeneration)
target_compile_definitions(RackSynthSpaceReverbRackTests PRIVATE RACKSYNTH_SPACE_BASELINE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/space-reverb-legacy")
racksynth_add_test(NAME RackSynthSpaceReverbRackTests COMMAND RackSynthSpaceReverbRackTests)
juce_add_console_app(RackSynthSpaceReverbWireTests PRODUCT_NAME "RackSynthSpaceReverbWireTests")
target_sources(RackSynthSpaceReverbWireTests PRIVATE tests/native/SpaceReverbWireTests.cpp)
target_link_libraries(RackSynthSpaceReverbWireTests PRIVATE RackSynthCompanionWire)
racksynth_add_test(NAME RackSynthSpaceReverbWireTests COMMAND RackSynthSpaceReverbWireTests)
juce_add_console_app(RackSynthSpaceReverbWetSmokeTests PRODUCT_NAME "RackSynthSpaceReverbWetSmokeTests")
target_sources(RackSynthSpaceReverbWetSmokeTests PRIVATE tests/native/SpaceReverbWetSmokeTests.cpp)
target_link_libraries(RackSynthSpaceReverbWetSmokeTests PRIVATE RackSynthState)
racksynth_add_test(NAME RackSynthSpaceReverbWetSmokeTests COMMAND RackSynthSpaceReverbWetSmokeTests)
juce_add_console_app(RackSynthSpaceReverbLifecycleTests PRODUCT_NAME "RackSynthSpaceReverbLifecycleTests")
target_sources(RackSynthSpaceReverbLifecycleTests PRIVATE tests/native/SpaceReverbLifecycleTests.cpp)
target_link_libraries(RackSynthSpaceReverbLifecycleTests PRIVATE RackSynthSpaceReverbActivation RackSynthRouted)
racksynth_add_test(NAME RackSynthSpaceReverbLifecycleTests COMMAND RackSynthSpaceReverbLifecycleTests)
juce_add_console_app(RackSynthSpaceReverbRoutingTests PRODUCT_NAME "RackSynthSpaceReverbRoutingTests")
target_sources(RackSynthSpaceReverbRoutingTests PRIVATE tests/native/SpaceReverbRoutingTests.cpp)
target_link_libraries(RackSynthSpaceReverbRoutingTests PRIVATE RackSynthRouted)
racksynth_add_test(NAME RackSynthSpaceReverbRoutingTests COMMAND RackSynthSpaceReverbRoutingTests)
juce_add_console_app(RackSynthSpaceReverbProcessorTests PRODUCT_NAME "RackSynthSpaceReverbProcessorTests")
target_sources(RackSynthSpaceReverbProcessorTests PRIVATE tests/native/SpaceReverbProcessorTests.cpp)
target_link_libraries(RackSynthSpaceReverbProcessorTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthSpaceReverbProcessorTests COMMAND RackSynthSpaceReverbProcessorTests)
if(RACKSYNTH_BUILD_PROBES)
juce_add_console_app(RackSynthSpaceReverbProductTests PRODUCT_NAME "RackSynthSpaceReverbProductTests")
target_sources(RackSynthSpaceReverbProductTests PRIVATE tests/native/SpaceReverbProductTests.cpp)
target_link_libraries(RackSynthSpaceReverbProductTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthSpaceReverbPeriodicCaller)
racksynth_add_test(NAME RackSynthSpaceReverbProductTests COMMAND RackSynthSpaceReverbProductTests)
target_include_directories(RackSynthSpaceReverbProductTests PRIVATE probes/space-reverb)
racksynth_add_test(NAME RackSynthSpaceReverbProductReaderSelfTest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/space_reverb_product_readback.py" --self-test)
endif()
target_sources(RackSynthInstrument PRIVATE src/studio/SpaceReverbGraph.cpp)
juce_add_console_app(RackSynthSpaceReverbGraphTests PRODUCT_NAME "RackSynthSpaceReverbGraphTests")
target_sources(RackSynthSpaceReverbGraphTests PRIVATE tests/native/SpaceReverbGraphTests.cpp)
if(TARGET RackSynthRackPackageFixtures)
    add_dependencies(RackSynthSpaceReverbGraphTests RackSynthRackPackageFixtures)
endif()
target_link_libraries(RackSynthSpaceReverbGraphTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
racksynth_add_test(NAME RackSynthSpaceReverbGraphTests COMMAND RackSynthSpaceReverbGraphTests)
set_tests_properties(RackSynthSpaceReverbGraphTests PROPERTIES RESOURCE_LOCK desktop)
juce_add_console_app(RackSynthEraReverbProductTests PRODUCT_NAME "RackSynthEraReverbProductTests")
target_sources(RackSynthEraReverbProductTests PRIVATE tests/native/EraReverbProductTests.cpp)
target_link_libraries(RackSynthEraReverbProductTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
target_compile_definitions(RackSynthEraReverbProductTests PRIVATE RACKSYNTH_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
racksynth_add_test(NAME RackSynthEraReverbProductTests_legacy COMMAND RackSynthEraReverbProductTests --legacy)
racksynth_add_test(NAME RackSynthEraReverbProductTests_state COMMAND RackSynthEraReverbProductTests --state)
racksynth_add_test(NAME RackSynthEraReverbProductTests_catalog COMMAND RackSynthEraReverbProductTests --catalog)
racksynth_add_test(NAME RackSynthEraReverbProductTests_studio COMMAND RackSynthEraReverbProductTests --studio)
racksynth_add_test(NAME RackSynthEraReverbProductTests_routing COMMAND RackSynthEraReverbProductTests --routing)

if(WIN32)
    add_library(RackSynthDrumGeneration STATIC
        src/drums/ComfyAudioJob.cpp src/drums/ComfyDrumPreparation.cpp)
    target_include_directories(RackSynthDrumGeneration PUBLIC src/drums)
    target_link_libraries(RackSynthDrumGeneration PRIVATE
        RackSynthComfyHttp RackSynthDrumPreparation RackSynthJson RackSynthJuce)
    target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentDrumGeneration.cpp)
    target_link_libraries(RackSynthInstrument PRIVATE RackSynthDrumGeneration)
    add_executable(RackSynthComfyAudioJobTests tests/native/ComfyAudioJobTests.cpp)
    target_link_libraries(RackSynthComfyAudioJobTests PRIVATE RackSynthDrumGeneration
        RackSynthComfyHttp RackSynthDrumPreparation RackSynthJson RackSynthJuce ws2_32)
    racksynth_add_test(NAME RackSynthComfyAudioJobTests
        COMMAND RackSynthComfyAudioJobTests --section all)
    add_executable(RackSynthComfyDrumPreparationTests tests/native/ComfyDrumPreparationTests.cpp)
    target_link_libraries(RackSynthComfyDrumPreparationTests PRIVATE RackSynthDrumGeneration
        RackSynthComfyHttp RackSynthDrumPreparation RackSynthJson RackSynthJuce ws2_32)
    racksynth_add_test(NAME RackSynthComfyDrumPreparationTests
        COMMAND RackSynthComfyDrumPreparationTests --section all)
    juce_add_console_app(RackSynthDrumGenerationTests PRODUCT_NAME "RackSynthDrumGenerationTests")
    target_sources(RackSynthDrumGenerationTests PRIVATE tests/native/DrumGenerationTests.cpp
        tests/native/PostedClick.cpp)
    target_compile_definitions(RackSynthDrumGenerationTests PRIVATE
        RACKSYNTH_WORKSPACE="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RackSynthDrumGenerationTests PRIVATE RackSynthJuceInstrumentApps
        RackSynthInstrument RackSynthDrumGeneration RackSynthComfyHttp
        RackSynthDrumPreparation RackSynthJson ws2_32)
    foreach(section IN ITEMS workflow lifetime instances)
        racksynth_add_test(NAME RackSynthDrumGenerationTests_${section}
            COMMAND RackSynthDrumGenerationTests --section ${section})
    endforeach()
    racksynth_add_test(NAME RackSynthDrumGenerationTests_desktop
        COMMAND RackSynthDrumGenerationTests --section desktop)
    set_tests_properties(RackSynthDrumGenerationTests_desktop PROPERTIES RESOURCE_LOCK desktop)
endif()

if(WIN32)
    target_sources(RackSynthAiProposal PRIVATE src/ai/LocalPreview.cpp)
    target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentLocalPreview.cpp)
    target_link_libraries(RackSynthInstrument PRIVATE RackSynthAiRender)
    target_include_directories(RackSynthAiProposal PUBLIC probes/oauth)
    juce_add_console_app(RackSynthDrumLocalPreviewTests PRODUCT_NAME "RackSynthDrumLocalPreviewTests")
    target_sources(RackSynthDrumLocalPreviewTests PRIVATE tests/native/DrumLocalPreviewTests.cpp)
    if(TARGET RackSynthRackPackageFixtures)
        add_dependencies(RackSynthDrumGenerationTests RackSynthRackPackageFixtures)
        add_dependencies(RackSynthDrumLocalPreviewTests RackSynthRackPackageFixtures)
    endif()
    target_link_libraries(RackSynthDrumLocalPreviewTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthCompanionRuntimeServer RackSynthCompanionBackend RackSynthAiGeneration RackSynthCompanionWire RackSynthDrumAssets)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_protocol COMMAND RackSynthDrumLocalPreviewTests --protocol)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_workflow COMMAND RackSynthDrumLocalPreviewTests --workflow)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_saturation COMMAND RackSynthDrumLocalPreviewTests --saturation)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_refine COMMAND RackSynthDrumLocalPreviewTests --refine)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_audition_default COMMAND RackSynthDrumLocalPreviewTests --audition-default)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_capture COMMAND RackSynthDrumLocalPreviewTests --capture)
    set_tests_properties(RackSynthDrumLocalPreviewTests_capture PROPERTIES RESOURCE_LOCK desktop)
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_lifetime COMMAND RackSynthDrumLocalPreviewTests --lifetime)
    set_tests_properties(RackSynthDrumLocalPreviewTests_lifetime PROPERTIES DISABLED "$<NOT:$<CONFIG:Debug>>")
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_oom_terminal COMMAND RackSynthDrumLocalPreviewTests --oom-terminal)
    set_tests_properties(RackSynthDrumLocalPreviewTests_oom_terminal PROPERTIES DISABLED "$<NOT:$<CONFIG:Debug>>")
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_observation COMMAND RackSynthDrumLocalPreviewTests --observation)
    set_tests_properties(RackSynthDrumLocalPreviewTests_observation PROPERTIES DISABLED "$<NOT:$<CONFIG:Debug>>")
    racksynth_add_test(NAME RackSynthDrumLocalPreviewTests_load COMMAND RackSynthDrumLocalPreviewTests --load)
    set_tests_properties(RackSynthDrumLocalPreviewTests_load PROPERTIES DISABLED "$<NOT:$<CONFIG:Debug>>")
endif()

get_directory_property(racksynth_tests TESTS)
if(WIN32)
    # The launcher enforces the unchanged 600s runtime after at most 1800s in the resource queue.
    set_tests_properties(${racksynth_tests} PROPERTIES TIMEOUT 2410)
else()
    set_tests_properties(${racksynth_tests} PROPERTIES TIMEOUT 600)
endif()
# Windows launchers isolate these on private desktops; other platforms keep the local desktop lock.
set_tests_properties(RackSynthInstrumentTests_complete_editor RackSynthPatternEditorTests RackSynthRackPanelTests RackSynthRackPanelTests_focus
    RackSynthStudioWindowTests_units RackSynthStudioWindowTests_stack RackSynthStudioWindowTests_window RackSynthMakerFaceTests RackSynthRackPackageRecallTests RackSynthDynamicEqGraphTests RackSynthSaturationGraphTests RackSynthDynamicEqProcessorTests PROPERTIES RESOURCE_LOCK desktop)
foreach(desktop_test IN ITEMS RackSynthAiPanelTests RackSynthGroupPanelTests RackSynthProgrammerTests
    RackSynthInstrumentTests RackSynthInstrumentAiTests RackSynthAiWorkflowSelfTest RackSynthGroupWorkflowSelfTest)
    if(TEST ${desktop_test})
        set_tests_properties(${desktop_test} PROPERTIES RESOURCE_LOCK desktop)
    endif()
endforeach()
# Debug binaries write generation diagnostics to the user's real log folder unless this is set. Each test has its own:
# a test reads back the lines it wrote, and the log's cross-process lock drops a write that meets another.
foreach(test IN LISTS racksynth_tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT=${CMAKE_BINARY_DIR}/test-diagnostics/${test}")
endforeach()

racksynth_private_desktops(${racksynth_tests})
if(WIN32)
    # Leave headroom for the long CPU cases within the existing sixteen-slot CI schedule.
    set_tests_properties(RackSynthProjectTests RackSynthRackPanelTests RackSynthRackPanelTests_focus RackSynthRoutedTests_modern_delay
        RackSynthRackInventoryBoundariesTests RackSynthRackInventoryCanonicalOverflowTests
        RackSynthStudioWindowTests_units RackSynthStudioWindowTests_stack RackSynthStudioWindowTests_window
        PROPERTIES PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS})
    # Keep previously reviewed long-case scheduling until measured concurrency supports a reduction.
    set_tests_properties(RackSynthAiGenerationTests RackSynthInstrumentTests_complete_editor
        RackSynthStateTests RackSynthCompanionWireTests PROPERTIES PROCESSORS 16)
endif()
if(WIN32 AND RACKSYNTH_BUILD_PROBES)
    # Synthetic children use private locks, while this parent participates in real activity admission.
    racksynth_add_test(NAME RackSynthNativeRunTests COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_SOURCE_DIR}/tests" -p test_native_run.py -v)
    set_tests_properties(RackSynthNativeRunTests PROPERTIES TIMEOUT 2410
        ENVIRONMENT "AIRACK_NATIVE_RUNNER=$<TARGET_FILE:RackSynthNativeRun>")
endif()
get_directory_property(all_native_tests TESTS)
racksynth_order_timing_tests(${all_native_tests})
if(WIN32)
    foreach(test IN LISTS all_native_tests)
        get_test_property(${test} RACKSYNTH_COORDINATED coordinated)
        if(NOT coordinated)
            message(FATAL_ERROR "Register ${test} with racksynth_add_test so it coordinates across worktrees")
        endif()
    endforeach()
endif()

if(WIN32)
    add_library(RackSynthComfyHttp STATIC src/companion/ComfyHttpWindows.cpp)
    target_include_directories(RackSynthComfyHttp PUBLIC src/companion)
    target_link_libraries(RackSynthComfyHttp PUBLIC RackSynthJson RackSynthJuce PRIVATE winhttp)
    add_executable(RackSynthComfyHttpWindowsTests tests/native/ComfyHttpWindowsTests.cpp)
    target_link_libraries(RackSynthComfyHttpWindowsTests PRIVATE RackSynthComfyHttp ws2_32)
    racksynth_add_test(NAME RackSynthComfyHttpWindowsTests COMMAND RackSynthComfyHttpWindowsTests)
    set_tests_properties(RackSynthComfyHttpWindowsTests PROPERTIES PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS})
    add_library(RackSynthComfyHttpUnloadTest SHARED tests/native/ComfyHttpUnloadTest.cpp)
    target_link_libraries(RackSynthComfyHttpUnloadTest PRIVATE RackSynthComfyHttp)
    add_dependencies(RackSynthComfyHttpWindowsTests RackSynthComfyHttpUnloadTest)
    target_compile_definitions(RackSynthComfyHttpWindowsTests PRIVATE
        RACKSYNTH_COMFY_HTTP_UNLOAD_TEST_DLL=L\"$<TARGET_FILE:RackSynthComfyHttpUnloadTest>\")
endif()

target_sources(RackSynthDrumAssets PRIVATE src/drums/DrumOwnership.cpp)
target_sources(RackSynthInstrument PRIVATE src/plugin/InstrumentDrums.cpp)
juce_add_console_app(RackSynthDrumOwnershipTests PRODUCT_NAME "RackSynthDrumOwnershipTests")
target_sources(RackSynthDrumOwnershipTests PRIVATE tests/native/DrumOwnershipTests.cpp)
target_link_libraries(RackSynthDrumOwnershipTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthDrumAssets)
racksynth_add_test(NAME RackSynthDrumOwnershipTests_core COMMAND RackSynthDrumOwnershipTests --ownership-core)
racksynth_add_test(NAME RackSynthDrumOwnershipTests_history COMMAND RackSynthDrumOwnershipTests --ownership-history)
racksynth_add_test(NAME RackSynthDrumOwnershipTests_recall COMMAND RackSynthDrumOwnershipTests --ownership-recall)
racksynth_add_test(NAME RackSynthDrumOwnershipTests_races COMMAND RackSynthDrumOwnershipTests --ownership-races)
if(WIN32)
    racksynth_add_test(NAME RackSynthDrumOwnershipTests_local_preview COMMAND RackSynthDrumOwnershipTests --ownership-local-preview)
    set_tests_properties(RackSynthDrumOwnershipTests_local_preview PROPERTIES TIMEOUT 2410
        DISABLED "$<NOT:$<CONFIG:Debug>>"
        ENVIRONMENT "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT=${CMAKE_BINARY_DIR}/test-diagnostics/RackSynthDrumOwnershipTests_local_preview")
endif()

juce_add_console_app(RackSynthDrumPerformanceTests PRODUCT_NAME "RackSynthDrumPerformanceTests")
target_sources(RackSynthDrumPerformanceTests PRIVATE tests/native/DrumPerformanceTests.cpp)
target_link_libraries(RackSynthDrumPerformanceTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthDrumAssets)
racksynth_add_test(NAME RackSynthDrumPerformanceTests_core COMMAND RackSynthDrumPerformanceTests --drum-performance-core)
racksynth_add_test(NAME RackSynthDrumPerformanceTests_lifecycle COMMAND RackSynthDrumPerformanceTests --drum-performance-lifecycle)
racksynth_add_test(NAME RackSynthDrumPerformanceTests_ranges COMMAND RackSynthDrumPerformanceTests --drum-performance-ranges)
racksynth_add_test(NAME RackSynthDrumPerformanceTests_heap COMMAND RackSynthDrumPerformanceTests --drum-performance-heap)

juce_add_console_app(RackSynthTileArrayTests PRODUCT_NAME "RackSynthTileArrayTests")
target_sources(RackSynthTileArrayTests PRIVATE tests/native/TileArrayTests.cpp)
target_link_libraries(RackSynthTileArrayTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument)
if(TARGET RackSynthRackPackageFixtures)
    add_dependencies(RackSynthTileArrayTests RackSynthRackPackageFixtures)
endif()
racksynth_add_test(NAME RackSynthTileArrayTests_face COMMAND RackSynthTileArrayTests --face)
racksynth_add_test(NAME RackSynthTileArrayTests_desktop COMMAND RackSynthTileArrayTests --desktop)
juce_add_console_app(RackSynthDrumImportTests PRODUCT_NAME "RackSynthDrumImportTests")
target_sources(RackSynthDrumImportTests PRIVATE tests/native/DrumImportTests.cpp)
target_link_libraries(RackSynthDrumImportTests PRIVATE RackSynthJuceInstrumentApps RackSynthInstrument RackSynthDrumPreparation)
racksynth_add_test(NAME RackSynthDrumImportTests_workflow COMMAND RackSynthDrumImportTests --workflow)
racksynth_add_test(NAME RackSynthDrumImportTests_lifetime COMMAND RackSynthDrumImportTests --lifetime)
set(tile_functional_tests RackSynthTileArrayTests_face RackSynthTileArrayTests_desktop RackSynthDrumImportTests_workflow RackSynthDrumImportTests_lifetime)
if(WIN32)
    set_tests_properties(${tile_functional_tests} PROPERTIES TIMEOUT 2410)
else()
    set_tests_properties(${tile_functional_tests} PROPERTIES TIMEOUT 600)
endif()
foreach(test IN LISTS tile_functional_tests)
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT=${CMAKE_BINARY_DIR}/test-diagnostics/${test}")
endforeach()
set_tests_properties(RackSynthTileArrayTests_face RackSynthTileArrayTests_desktop RackSynthDrumImportTests_workflow PROPERTIES RESOURCE_LOCK desktop)
racksynth_private_desktops(${tile_functional_tests})

target_sources(RackSynthInstrument PRIVATE src/plugin/DrumImport.cpp src/studio/DrumSourcePopup.cpp)
target_link_libraries(RackSynthInstrument PRIVATE RackSynthDrumPreparation)

target_sources(RackSynthInstrument PRIVATE src/studio/TileArrayFace.cpp)

juce_add_console_app(RackSynthDrumPlaybackQualityTests PRODUCT_NAME "RackSynthDrumPlaybackQualityTests")
target_sources(RackSynthDrumPlaybackQualityTests PRIVATE tests/native/DrumPlaybackQualityTests.cpp)
target_link_libraries(RackSynthDrumPlaybackQualityTests PRIVATE RackSynthDrumEngine RackSynthRouted)
racksynth_add_test(NAME RackSynthDrumPlaybackQualityTests COMMAND RackSynthDrumPlaybackQualityTests)
