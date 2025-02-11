//
// Copyright 2017 Valve Corporation. All rights reserved. Subject to the following license:
// https://valvesoftware.github.io/steam-audio/license.html
//

#include "steamaudio_fmod.h"

#if defined(IPL_OS_MACOSX)
#include <mach-o/dyld.h>
#endif

#if defined(IPL_OS_IOS)
#define STEAMAUDIO_SKIP_API_FUNCTIONS
#endif
#include <phonon_interfaces.h>


namespace SteamAudioFMOD {

// --------------------------------------------------------------------------------------------------------------------
// Global State
// --------------------------------------------------------------------------------------------------------------------

IPLContext gContext = nullptr;
IPLHRTF gHRTF[2] = { nullptr, nullptr };
IPLSimulationSettings gSimulationSettings;
IPLSource gReverbSource[2] = { nullptr, nullptr };
IPLReflectionMixer gReflectionMixer[2] = { nullptr, nullptr };

std::atomic<bool> gNewHRTFWritten{ false };
std::atomic<bool> gIsSimulationSettingsValid{ false };
std::atomic<bool> gNewReverbSourceWritten{ false };
std::atomic<bool> gNewReflectionMixerWritten{ false };

std::shared_ptr<SourceManager> gSourceManager;


// --------------------------------------------------------------------------------------------------------------------
// Helper Functions
// --------------------------------------------------------------------------------------------------------------------

IPLSpeakerLayout speakerLayoutForNumChannels(int numChannels)
{
    IPLSpeakerLayout speakerLayout;
    speakerLayout.numSpeakers = numChannels;
    speakerLayout.speakers = nullptr;
    
    if (numChannels == 1)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_MONO;
    else if (numChannels == 2)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    else if (numChannels == 4)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_QUADRAPHONIC;
    else if (numChannels == 6)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_5_1;
    else if (numChannels == 8)
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1;
    else
        speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_CUSTOM;
    
    return speakerLayout;
}

int orderForNumChannels(int numChannels)
{
    return static_cast<int>(sqrtf(static_cast<float>(numChannels))) - 1;
}

int numChannelsForOrder(int order)
{
    return (order + 1) * (order + 1);
}

int numSamplesForDuration(float duration,
                          int samplingRate)
{
    return static_cast<int>(ceilf(duration * samplingRate));
}

IPLVector3 convertVector(float x,
                         float y,
                         float z)
{
    return IPLVector3{ x, y, -z };
}

IPLVector3 unitVector(IPLVector3 v)
{
    auto length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-2f)
        length = 1e-2f;
    
    return IPLVector3{v.x / length, v.y / length, v.z / length};
}

float dot(const IPLVector3& a,
          const IPLVector3& b)
{
    return (a.x * b.x + a.y * b.y + a.z * b.z);
}

IPLVector3 cross(const IPLVector3& a,
                 const IPLVector3& b)
{
    IPLVector3 c;
    c.x = a.y * b.z - a.z * b.y;
    c.y = a.z * b.x - a.x * b.z;
    c.z = a.x * b.y - a.y * b.x;
    return c;
}

float distance(const IPLVector3& a,
               const IPLVector3& b)
{
    auto d = IPLVector3{a.x - b.x, a.y - b.y, a.z - b.z};
    return sqrtf(dot(d, d));
}

void applyVolumeRamp(float startVolume,
                     float endVolume,
                     int numSamples,
                     float* buffer)
{
    for (auto i = 0; i < numSamples; ++i)
    {
        auto fraction = static_cast<float>(i) / static_cast<float>(numSamples);
        auto volume = fraction * endVolume + (1.0f - fraction) * startVolume;
        
        buffer[i] *= volume;
    }
}

IPLCoordinateSpace3 calcCoordinates(const FMOD_3D_ATTRIBUTES& attributes)
{
    IPLCoordinateSpace3 coordinates;
    coordinates.ahead = convertVector(attributes.forward.x, attributes.forward.y, attributes.forward.z);
    coordinates.up = convertVector(attributes.up.x, attributes.up.y, attributes.up.z);
    coordinates.right = unitVector(cross(coordinates.ahead, coordinates.up));
    coordinates.origin = convertVector(attributes.position.x, attributes.position.y, attributes.position.z);
    return coordinates;
}

IPLCoordinateSpace3 calcListenerCoordinates(FMOD_DSP_STATE* state)
{
    auto numListeners = 1;
    FMOD_3D_ATTRIBUTES listenerAttributes;
    state->functions->getlistenerattributes(state, &numListeners, &listenerAttributes);
    
    return calcCoordinates(listenerAttributes);
}

bool isRunningInEditor()
{
#if defined(IPL_OS_WINDOWS)
    wchar_t moduleFileName[MAX_PATH] = {0};
    GetModuleFileName(nullptr, moduleFileName, MAX_PATH);
    return (wcsstr(moduleFileName, L"FMOD Studio.exe") != nullptr);
#elif defined(IPL_OS_MACOSX)
    char moduleFileName[1024] = {0};
    uint32_t bufferSize = 1024;
    _NSGetExecutablePath(moduleFileName, &bufferSize);
    return (strstr(moduleFileName, "FMOD Studio.app") != nullptr);
#endif
}

void initContextAndDefaultHRTF(IPLAudioSettings audioSettings)
{
    IPLContextSettings contextSettings{};
    contextSettings.version = STEAMAUDIO_VERSION;
    contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2;
    
    IPLContext context = nullptr;
    IPL_API(iplContextCreate(&contextSettings, &context));
    
    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    
    IPLHRTF hrtf = nullptr;
    iplHRTFCreate(context, &audioSettings, &hrtfSettings, &hrtf);
    
    iplFMODInitialize(context);
    iplFMODSetHRTF(hrtf);
    
    iplHRTFRelease(&hrtf);
    iplContextRelease(&context);
}


// --------------------------------------------------------------------------------------------------------------------
// SourceManager
// --------------------------------------------------------------------------------------------------------------------

SourceManager::SourceManager()
: mNextHandle(0)
{}

SourceManager::~SourceManager()
{
    {
        std::lock_guard<std::mutex> lock(mSourceMutex);
        for (auto& it : mSources)
        {
            iplSourceRelease(&mSources[it.first]);
        }
    }
}

int32_t SourceManager::addSource(IPLSource source)
{
    // Retain a reference to this source.
    auto sourceRetained = iplSourceRetain(source);
    
    auto handle = -1;
    
    // First, figure out the handle we want to use.
    {
        std::lock_guard<std::mutex> lock(mHandleMutex);
        
        if (mFreeHandles.empty())
        {
            // No free handles, use the next-available unused handle.
            handle = mNextHandle++;
        }
        else
        {
            // Use one of the free handles.
            handle = mFreeHandles.top();
            mFreeHandles.pop();
        }
    }
    
    assert(handle >= 0);
    
    // Now store the mapping from the handle to this source.
    {
        std::lock_guard<std::mutex> lock(mSourceMutex);
        
        assert(mSources.find(handle) == mSources.end());
        
        mSources[handle] = sourceRetained;
    }
    
    return handle;
}

void SourceManager::removeSource(int32_t handle)
{
    // Remove the source from the handle-to-source map.
    {
        std::lock_guard<std::mutex> lock(mSourceMutex);
        
        if (mSources.find(handle) != mSources.end())
        {
            iplSourceRelease(&mSources[handle]);
            mSources.erase(handle);
        }
    }
    
    // Mark the handle as free.
    {
        std::lock_guard<std::mutex> lock(mHandleMutex);
        
        mFreeHandles.push(handle);
    }
}

IPLSource SourceManager::getSource(int32_t handle)
{
    std::lock_guard<std::mutex> lock(mSourceMutex);
    
    if (mSources.find(handle) != mSources.end())
        return mSources[handle];
    else
        return nullptr;
}

extern FMOD_DSP_DESCRIPTION gSpatializeEffect;
extern FMOD_DSP_DESCRIPTION gMixerReturnEffect;
extern FMOD_DSP_DESCRIPTION gReverbEffect;

static FMOD_PLUGINLIST gPluginList[] =
{
    { FMOD_PLUGINTYPE_DSP, &gSpatializeEffect },
    { FMOD_PLUGINTYPE_DSP, &gMixerReturnEffect },
    { FMOD_PLUGINTYPE_DSP, &gReverbEffect },
    { FMOD_PLUGINTYPE_MAX, nullptr }
};

namespace SpatializeEffect { extern void initParamDescs(); }
namespace MixerReturnEffect { extern void initParamDescs(); }
namespace ReverbEffect { extern void initParamDescs(); }

}


// --------------------------------------------------------------------------------------------------------------------
// API Functions
// --------------------------------------------------------------------------------------------------------------------

using namespace SteamAudioFMOD;

FMOD_PLUGINLIST * F_CALL FMODGetPluginDescriptionList()
{
    SpatializeEffect::initParamDescs();
    MixerReturnEffect::initParamDescs();
    ReverbEffect::initParamDescs();
    return gPluginList;
}

FMOD_DSP_DESCRIPTION* F_CALL FMOD_SteamAudio_Spatialize_GetDSPDescription()
{
    SteamAudioFMOD::SpatializeEffect::initParamDescs();
    return &SteamAudioFMOD::gSpatializeEffect;
}

FMOD_DSP_DESCRIPTION* F_CALL FMOD_SteamAudio_MixerReturn_GetDSPDescription()
{
    SteamAudioFMOD::MixerReturnEffect::initParamDescs();
    return &SteamAudioFMOD::gMixerReturnEffect;
}

FMOD_DSP_DESCRIPTION* F_CALL FMOD_SteamAudio_Reverb_GetDSPDescription()
{
    SteamAudioFMOD::ReverbEffect::initParamDescs();
    return &SteamAudioFMOD::gReverbEffect;
}

void F_CALL iplFMODGetVersion(unsigned int* major, 
                              unsigned int* minor, 
                              unsigned int* patch)
{
    if (major)
        *major = STEAMAUDIO_FMOD_VERSION_MAJOR;
    if (minor)
        *minor = STEAMAUDIO_FMOD_VERSION_MINOR;
    if (patch)
        *patch = STEAMAUDIO_FMOD_VERSION_PATCH;
}

void F_CALL iplFMODInitialize(IPLContext context)
{
    assert(gContext == nullptr);

    gContext = iplContextRetain(context);

    gSourceManager = std::make_shared<SourceManager>();
}

void F_CALL iplFMODTerminate()
{
    gNewReflectionMixerWritten = false;
    iplReflectionMixerRelease(&gReflectionMixer[0]);
    iplReflectionMixerRelease(&gReflectionMixer[1]);

    gNewReverbSourceWritten = false;
    iplSourceRelease(&gReverbSource[0]);
    iplSourceRelease(&gReverbSource[1]);

    gIsSimulationSettingsValid = false;

    gNewHRTFWritten = false;
    iplHRTFRelease(&gHRTF[0]);
    iplHRTFRelease(&gHRTF[1]);

    iplContextRelease(&gContext);

    gSourceManager = nullptr;
}

void F_CALL iplFMODSetHRTF(IPLHRTF hrtf)
{
    if (hrtf == gHRTF[1])
        return;

    if (!gNewHRTFWritten)
    {
        iplHRTFRelease(&gHRTF[1]);
        gHRTF[1] = iplHRTFRetain(hrtf);

        gNewHRTFWritten = true;
    }
}

void F_CALL iplFMODSetSimulationSettings(IPLSimulationSettings simulationSettings)
{
    gSimulationSettings = simulationSettings;

    gIsSimulationSettingsValid = true;
}

void F_CALL iplFMODSetReverbSource(IPLSource reverbSource)
{
    if (reverbSource == gReverbSource[1])
        return;

    if (!gNewReverbSourceWritten)
    {
        iplSourceRelease(&gReverbSource[1]);
        gReverbSource[1] = iplSourceRetain(reverbSource);
 
        gNewReverbSourceWritten = true;
    }
}

IPLint32 F_CALL iplFMODAddSource(IPLSource source)
{
    if (!gSourceManager)
        return -1;

    return gSourceManager->addSource(source);
}

void F_CALL iplFMODRemoveSource(IPLint32 handle)
{
    if (!gSourceManager)
        return;

    gSourceManager->removeSource(handle);
}
