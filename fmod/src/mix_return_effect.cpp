//
// Copyright 2017 Valve Corporation. All rights reserved. Subject to the following license:
// https://valvesoftware.github.io/steam-audio/license.html
//

#include "steamaudio_fmod.h"

namespace SteamAudioFMOD {

namespace MixerReturnEffect {

/**
 *  DSP parameters for the "Steam Audio Mixer Return" effect.
 */
enum Params
{
    /**
     *  **Type**: `FMOD_DSP_PARAMETER_TYPE_BOOL`
     *
     *  If true, applies HRTF-based 3D audio rendering to mixed reflected sound. Results in an improvement in 
     *  spatialization quality, at the cost of slightly increased CPU usage.
     */
    BINAURAL,

    /** The number of parameters in this effect. */
    NUM_PARAMS
};

FMOD_DSP_PARAMETER_DESC gParams[] = {
    { FMOD_DSP_PARAMETER_TYPE_BOOL, "Binaural", "", "Spatialize reflected sound using HRTF." }
};

FMOD_DSP_PARAMETER_DESC* gParamsArray[NUM_PARAMS];

void initParamDescs()
{
    for (auto i = 0; i < NUM_PARAMS; ++i)
    {
        gParamsArray[i] = &gParams[i];
    }

    gParams[BINAURAL].booldesc = {false};
}

struct State
{
    bool binaural;

    IPLAudioBuffer reflectionsBuffer;
    IPLAudioBuffer inBuffer;
    IPLAudioBuffer outBuffer;

    IPLReflectionMixer reflectionMixer;
    IPLAmbisonicsDecodeEffect ambisonicsEffect;
};

enum InitFlags
{
    INIT_NONE = 0,
    INIT_AUDIOBUFFERS = 1 << 0,
    INIT_REFLECTIONEFFECT = 1 << 1,
    INIT_AMBISONICSEFFECT = 1 << 2
};

InitFlags lazyInit(FMOD_DSP_STATE* state,
                   int numChannelsIn,
                   int numChannelsOut)
{
    auto initFlags = INIT_NONE;

    IPLAudioSettings audioSettings;
    state->functions->getsamplerate(state, &audioSettings.samplingRate);
    state->functions->getblocksize(state, reinterpret_cast<unsigned int*>(&audioSettings.frameSize));

    if (!gContext && isRunningInEditor())
    {
        initContextAndDefaultHRTF(audioSettings);
    }

    if (!gContext)
        return initFlags;

    if (!gHRTF[1])
        return initFlags;

    auto effect = reinterpret_cast<State*>(state->plugindata);

    auto status = IPL_STATUS_SUCCESS;

    if (gIsSimulationSettingsValid)
    {
        status = IPL_STATUS_SUCCESS;

        if (!effect->reflectionMixer)
        {
            IPLReflectionEffectSettings effectSettings;
            effectSettings.type = gSimulationSettings.reflectionType;
            effectSettings.numChannels = numChannelsForOrder(gSimulationSettings.maxOrder);

            status = iplReflectionMixerCreate(gContext, &audioSettings, &effectSettings, &effect->reflectionMixer);

            if (!gNewReflectionMixerWritten)
            {
                iplReflectionMixerRelease(&gReflectionMixer[1]);
                gReflectionMixer[1] = iplReflectionMixerRetain(effect->reflectionMixer);

                gNewReflectionMixerWritten = true;
            }
        }

        if (status == IPL_STATUS_SUCCESS)
            initFlags = static_cast<InitFlags>(initFlags | INIT_REFLECTIONEFFECT);
    }

    if (numChannelsOut > 0 && gIsSimulationSettingsValid)
    {
        status = IPL_STATUS_SUCCESS;

        if (!effect->ambisonicsEffect)
        {
            IPLAmbisonicsDecodeEffectSettings effectSettings;
            effectSettings.speakerLayout = speakerLayoutForNumChannels(numChannelsOut);
            effectSettings.hrtf = gHRTF[1];
            effectSettings.maxOrder = gSimulationSettings.maxOrder;

            status = iplAmbisonicsDecodeEffectCreate(gContext, &audioSettings, &effectSettings, &effect->ambisonicsEffect);
        }

        if (status == IPL_STATUS_SUCCESS)
            initFlags = static_cast<InitFlags>(initFlags | INIT_AMBISONICSEFFECT);
    }

    if (numChannelsIn > 0 && numChannelsOut > 0)
    {
        auto numAmbisonicChannels = numChannelsForOrder(gSimulationSettings.maxOrder);

        if (!effect->reflectionsBuffer.data)
            iplAudioBufferAllocate(gContext, numAmbisonicChannels, audioSettings.frameSize, &effect->reflectionsBuffer);

        if (!effect->inBuffer.data)
            iplAudioBufferAllocate(gContext, numChannelsIn, audioSettings.frameSize, &effect->inBuffer);

        if (!effect->outBuffer.data)
            iplAudioBufferAllocate(gContext, numChannelsOut, audioSettings.frameSize, &effect->outBuffer);

        initFlags = static_cast<InitFlags>(initFlags | INIT_AUDIOBUFFERS);
    }

    return initFlags;
}

void reset(FMOD_DSP_STATE* state)
{
    auto effect = reinterpret_cast<State*>(state->plugindata);
    if (!effect)
        return;

    effect->binaural = false;
}

FMOD_RESULT F_CALL create(FMOD_DSP_STATE* state)
{
    state->plugindata = new State();
    reset(state);
    lazyInit(state, 0, 0);
    return FMOD_OK;
}

FMOD_RESULT F_CALL release(FMOD_DSP_STATE* state)
{
    auto effect = reinterpret_cast<State*>(state->plugindata);

    iplAudioBufferFree(gContext, &effect->reflectionsBuffer);
    iplAudioBufferFree(gContext, &effect->inBuffer);
    iplAudioBufferFree(gContext, &effect->outBuffer);

    iplReflectionMixerRelease(&effect->reflectionMixer);
    iplAmbisonicsDecodeEffectRelease(&effect->ambisonicsEffect);

    delete state->plugindata;

    return FMOD_OK;
}

FMOD_RESULT F_CALL getBool(FMOD_DSP_STATE* state,
                           int index,
                           FMOD_BOOL* value,
                           char*)
{
    auto effect = reinterpret_cast<State*>(state->plugindata);

    switch (index)
    {
    case BINAURAL:
        *value = effect->binaural;
        break;
    default:
        return FMOD_ERR_INVALID_PARAM;
    }

    return FMOD_OK;
}

FMOD_RESULT F_CALL setBool(FMOD_DSP_STATE* state,
                           int index,
                           FMOD_BOOL value)
{
    auto effect = reinterpret_cast<State*>(state->plugindata);

    switch (index)
    {
    case BINAURAL:
        effect->binaural = value;
        break;
    default:
        return FMOD_ERR_INVALID_PARAM;
    }

    return FMOD_OK;
}

FMOD_RESULT F_CALL process(FMOD_DSP_STATE* state,
                           unsigned int length,
                           const FMOD_DSP_BUFFER_ARRAY* inBuffers,
                           FMOD_DSP_BUFFER_ARRAY* outBuffers,
                           FMOD_BOOL inputsIdle,
                           FMOD_DSP_PROCESS_OPERATION operation)
{
    if (operation == FMOD_DSP_PROCESS_QUERY)
    {
        if (inputsIdle)
            return FMOD_ERR_DSP_DONTPROCESS;
    }
    else if (operation == FMOD_DSP_PROCESS_PERFORM)
    {
        auto effect = reinterpret_cast<State*>(state->plugindata);

        auto samplingRate = 0;
        auto frameSize = 0u;
        state->functions->getsamplerate(state, &samplingRate);
        state->functions->getblocksize(state, &frameSize);

        auto numChannelsIn = inBuffers->buffernumchannels[0];
        auto numChannelsOut = outBuffers->buffernumchannels[0];
        auto in = inBuffers->buffers[0];
        auto out = outBuffers->buffers[0];

        // Start by clearing the output buffer.
        memset(out, 0, numChannelsOut * frameSize * sizeof(float));

        // Make sure that audio processing state has been initialized. If initialization fails, stop and emit silence.
        auto initFlags = lazyInit(state, numChannelsIn, numChannelsOut);
        if (!(initFlags & INIT_AUDIOBUFFERS) || !(initFlags & INIT_REFLECTIONEFFECT) || !(initFlags & INIT_AMBISONICSEFFECT))
            return FMOD_ERR_DSP_SILENCE;

        if (gNewHRTFWritten)
        {
            iplHRTFRelease(&gHRTF[0]);
            gHRTF[0] = iplHRTFRetain(gHRTF[1]);

            gNewHRTFWritten = false;
        }

        auto listenerCoordinates = calcListenerCoordinates(state);

        IPLReflectionEffectParams reflectionParams;
        reflectionParams.numChannels = numChannelsForOrder(gSimulationSettings.maxOrder);
        reflectionParams.tanDevice = gSimulationSettings.tanDevice;

        iplReflectionMixerApply(effect->reflectionMixer, &reflectionParams, &effect->reflectionsBuffer);

        IPLAmbisonicsDecodeEffectParams ambisonicsParams;
        ambisonicsParams.order = gSimulationSettings.maxOrder;
        ambisonicsParams.hrtf = gHRTF[0];
        ambisonicsParams.orientation = listenerCoordinates;
        ambisonicsParams.binaural = (effect->binaural) ? IPL_TRUE : IPL_FALSE;

        iplAmbisonicsDecodeEffectApply(effect->ambisonicsEffect, &ambisonicsParams, &effect->reflectionsBuffer, &effect->outBuffer);

        iplAudioBufferDeinterleave(gContext, in, &effect->inBuffer);
        iplAudioBufferMix(gContext, &effect->inBuffer, &effect->outBuffer);
        
        iplAudioBufferInterleave(gContext, &effect->outBuffer, out);

        return FMOD_OK;
    }

    return FMOD_OK;
}

}

/** Descriptor for the Mixer Return effect. */
FMOD_DSP_DESCRIPTION gMixerReturnEffect 
{
    FMOD_PLUGIN_SDK_VERSION,
    "Steam Audio Mixer Return",
    STEAMAUDIO_FMOD_VERSION,
    1, 
    1,
    MixerReturnEffect::create,
    MixerReturnEffect::release,
    nullptr,
    nullptr,
    MixerReturnEffect::process,
    nullptr,
    MixerReturnEffect::NUM_PARAMS,
    MixerReturnEffect::gParamsArray,
    nullptr,
    nullptr,
    MixerReturnEffect::setBool,
    nullptr,
    nullptr,
    nullptr,
    MixerReturnEffect::getBool,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

}
