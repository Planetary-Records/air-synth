add_library(RackSynthSpaceReverbModel STATIC ../SpaceReverbModel.cpp)
target_include_directories(RackSynthSpaceReverbModel PUBLIC ..)
target_compile_features(RackSynthSpaceReverbModel PUBLIC cxx_std_20)
add_library(RackSynthSpaceReverbControls STATIC ../SpaceReverbControls.cpp)
target_link_libraries(RackSynthSpaceReverbControls PUBLIC RackSynthSpaceReverbModel)
add_library(RackSynthSpaceReverbRuntime STATIC
    runtime/ModalConverter.cpp
    runtime/ModalKernel.cpp
    runtime/ModalLiveTail.cpp
    runtime/ModalOuter.cpp
    runtime/ModalRoom.cpp
    runtime/ModalService.cpp
    runtime/ModalTailMath.cpp
    runtime/ModalTank.cpp
)
target_include_directories(RackSynthSpaceReverbRuntime PUBLIC runtime ..)
target_link_libraries(RackSynthSpaceReverbRuntime PUBLIC RackSynthSpaceReverbControls)
if(MSVC)
    target_compile_options(RackSynthSpaceReverbRuntime PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /fp:strict)
endif()
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(RackSynthSpaceReverbRuntime PRIVATE runtime/ModalAvx2.cpp runtime/ModalConverterAvx2.cpp)
    target_compile_definitions(RackSynthSpaceReverbRuntime PRIVATE AIRACK_MODAL_AVX2=1 AIRACK_MODAL_CONVERTER_AVX2=1)
    set_source_files_properties(runtime/ModalAvx2.cpp runtime/ModalConverterAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:strict")
endif()

add_library(RackSynthSpaceReverbActivation STATIC SpaceActivation.cpp)
target_link_libraries(RackSynthSpaceReverbActivation PUBLIC RackSynthSpaceReverbRuntime)

if(RACKSYNTH_BUILD_PROBES)
add_library(RackSynthSpaceReverbLoss STATIC ../SpaceReverbLoss.cpp ../SpaceReverbTail.cpp)
target_link_libraries(RackSynthSpaceReverbLoss PUBLIC RackSynthSpaceReverbModel)
add_library(RackSynthSpaceReverbPacked STATIC ../SpaceReverbPacked.cpp)
target_link_libraries(RackSynthSpaceReverbPacked PUBLIC RackSynthSpaceReverbModel)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(RackSynthSpaceReverbPacked PRIVATE ../SpaceReverbAvx2.cpp)
    target_compile_definitions(RackSynthSpaceReverbPacked PRIVATE AIRACK_SPACE_AVX2=1)
    set_source_files_properties(../SpaceReverbAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:strict")
endif()
add_library(RackSynthSpaceReverbTankRuntime STATIC ../SpaceReverbTank.cpp)
target_link_libraries(RackSynthSpaceReverbTankRuntime PUBLIC RackSynthSpaceReverbLoss RackSynthSpaceReverbPacked)
add_library(RackSynthSpaceReverbField STATIC ../SpaceReverbField.cpp)
target_link_libraries(RackSynthSpaceReverbField PUBLIC RackSynthSpaceReverbTankRuntime RackSynthSpaceReverbControls)
endif()
