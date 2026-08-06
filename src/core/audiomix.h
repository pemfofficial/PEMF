// audiomix.h - a small mixer of our own, so PEMF can fade.
//
// Decodes an audio file to PCM once with Media Foundation, then loops it
// through XAudio2 where the volume is a real gain control.
//
// ------------------------------------------------------------ why not Miles
// The GAME's audio is Miles Sound System, and Miles can play mp3 -- the
// filename resolver at FUN_0052CDC0 builds `<base>.mp3` and probes for it. That
// remains the RIGHT home for PEMF sound: it would route through the game's own
// mixer, respect its volume slider and pause with it. What it needs is the
// by-name entry point for a clip that is NOT in the pre-registered sound table,
// which means the audio manager `this` plus the lower-level loader path, and
// that is still unfinished (see the audio notes in GAME_API.md).
//
// ------------------------------------------------------------- why not MCI
// The first attempt used MCI from winmm, which needs no RE at all. It plays the
// file correctly and cannot change its volume:
//
//     storm audio: volume check -- set 12, driver accepted, reads back 12 (12)
//
// It accepted `setaudio volume`, returned success, AND read the value back --
// while playing at full volume throughout. The return code proved nothing and
// the readback proved the driver had STORED the number, not that it had used
// it. Without gain there is no fade, which is the whole point.
//
// So: our own mixer. XAudio2's SetVolume is an actual gain, and a fade is then
// just a ramp. The cost is that this is not the game's mixer -- its volume
// slider does not reach us -- so anything played through here must be gated on
// game state by the caller.
#pragma once
#include <windows.h>
#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>

#include "log.h"

namespace audiomix {

// A decoded track, held in memory. These are short loops, so the whole thing
// lives as one PCM buffer rather than streaming -- simpler, and it makes
// seamless looping XAudio2's problem instead of ours.
struct Track {
    BYTE*  pcm     = nullptr;
    UINT32 bytes   = 0;
    WAVEFORMATEX  fmt = {};
    IXAudio2SourceVoice* voice = nullptr;
    bool   playing = false;
    float  level   = 0.0f;      // 0..1, what we have ramped to
};

inline IXAudio2*               g_xa     = nullptr;
inline IXAudio2MasteringVoice* g_master = nullptr;
inline bool g_ready  = false;
inline bool g_failed = false;     // tried and could not; do not retry per frame
inline bool g_mfUp   = false;
inline bool g_comUp  = false;

inline void Shutdown();

// ---------------------------------------------------------------- start-up
inline bool Init()
{
    if (g_ready)  return true;
    if (g_failed) return false;

    // The game thread may already have COM up; either answer is fine, we only
    // need to know whether WE were the ones who started it.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hrCom == S_OK) g_comUp = true;
    else if (hrCom != S_FALSE && hrCom != RPC_E_CHANGED_MODE) {
        Log("audiomix: CoInitializeEx failed (0x%08X)", (unsigned)hrCom);
        g_failed = true;
        return false;
    }

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        Log("audiomix: MFStartup failed (0x%08X) -- no decoder, no music",
            (unsigned)hr);
        g_failed = true;
        return false;
    }
    g_mfUp = true;

    hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        Log("audiomix: XAudio2Create failed (0x%08X)", (unsigned)hr);
        g_failed = true;
        Shutdown();
        return false;
    }
    hr = g_xa->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) {
        Log("audiomix: CreateMasteringVoice failed (0x%08X)", (unsigned)hr);
        g_failed = true;
        Shutdown();
        return false;
    }

    g_ready = true;
    Log("audiomix: ready");
    return true;
}

inline void Shutdown()
{
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xa)     { g_xa->Release();          g_xa = nullptr; }
    if (g_mfUp)   { MFShutdown();             g_mfUp = false; }
    if (g_comUp)  { CoUninitialize();         g_comUp = false; }
    g_ready = false;
}

// ------------------------------------------------------------------ decode
// Whole-file decode to 16-bit PCM. Media Foundation handles mp3, wav and
// anything else the machine has a decoder for, so the player can drop in
// whatever they like.
inline bool Load(Track& t, const char* path)
{
    if (!Init()) return false;
    if (t.pcm)   return true;          // already loaded

    wchar_t wide[MAX_PATH] = {0};
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) == 0) return false;

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(wide, nullptr, &reader);
    if (FAILED(hr)) {
        Log("audiomix: cannot open %s (0x%08X)", path, (unsigned)hr);
        return false;
    }

    // Ask for uncompressed PCM and let MF insert whatever decoder it needs.
    IMFMediaType* want = nullptr;
    hr = MFCreateMediaType(&want);
    if (SUCCEEDED(hr)) {
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                         nullptr, want);
        want->Release();
    }
    if (FAILED(hr)) {
        Log("audiomix: no PCM conversion for %s (0x%08X)", path, (unsigned)hr);
        reader->Release();
        return false;
    }

    // Read back what we actually got -- sample rate and channel count come from
    // the file, not from us.
    IMFMediaType* got = nullptr;
    hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got);
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX* wf = nullptr;
        UINT32 wfSize = 0;
        hr = MFCreateWaveFormatExFromMFMediaType(got, &wf, &wfSize);
        if (SUCCEEDED(hr) && wf) {
            t.fmt = *wf;
            t.fmt.cbSize = 0;
            CoTaskMemFree(wf);
        }
        got->Release();
    }
    if (FAILED(hr) || t.fmt.nChannels == 0) {
        Log("audiomix: could not read the format of %s", path);
        reader->Release();
        return false;
    }

    // Pull every sample into one growing buffer.
    UINT32 cap = 1 << 20, len = 0;
    BYTE*  buf = (BYTE*)malloc(cap);
    if (!buf) { reader->Release(); return false; }

    for (;;) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (sample) sample->Release(); break; }
        if (!sample) continue;

        IMFMediaBuffer* mb = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mb)) && mb) {
            BYTE* p = nullptr; DWORD cur = 0;
            if (SUCCEEDED(mb->Lock(&p, nullptr, &cur))) {
                if (len + cur > cap) {
                    while (len + cur > cap) cap *= 2;
                    BYTE* grown = (BYTE*)realloc(buf, cap);
                    if (!grown) { mb->Unlock(); mb->Release(); sample->Release();
                                  free(buf); reader->Release(); return false; }
                    buf = grown;
                }
                memcpy(buf + len, p, cur);
                len += cur;
                mb->Unlock();
            }
            mb->Release();
        }
        sample->Release();
    }
    reader->Release();

    if (len == 0) {
        Log("audiomix: %s decoded to nothing", path);
        free(buf);
        return false;
    }

    t.pcm   = buf;
    t.bytes = len;
    Log("audiomix: decoded %s -- %u bytes, %u Hz, %u ch",
        path, len, t.fmt.nSamplesPerSec, t.fmt.nChannels);
    return true;
}

// ------------------------------------------------------------------ voice
// ⚠️ `t.playing` IS OUR BELIEF, NOT THE VOICE'S STATE, and the two can part
// company. XAudio2 stops a source voice on its own when it runs out of queued
// buffers -- and once that happens while our flag still says "playing",
// everything downstream is wrong: the caller never restarts it because it
// thinks it is already running, and a stop it never issued is never issued.
//
// Reported from play as "the drums start, fade out once, and never come back
// even in a new storm", which is exactly the shape of that disagreement.
//
// So ask the voice. This reconciles the flag with reality and is the only thing
// callers should test.
inline bool Playing(Track& t)
{
    if (!t.playing) return false;
    if (!t.voice)   { t.playing = false; return false; }

    XAUDIO2_VOICE_STATE st = {};
    t.voice->GetState(&st);
    if (st.BuffersQueued == 0) {
        // It ran dry. Our flag was stale; say so once and correct it.
        Log("audiomix: the voice ran out of buffers -- clearing a stale "
            "playing flag so it can start again");
        t.playing = false;
        return false;
    }
    return true;
}

inline bool Play(Track& t)
{
    // Playing() rather than t.playing: a stale flag would make this a no-op
    // for the rest of the session.
    if (Playing(t) || !t.pcm || !g_ready) return t.playing;

    if (!t.voice) {
        const HRESULT hr = g_xa->CreateSourceVoice(&t.voice, &t.fmt);
        if (FAILED(hr) || !t.voice) {
            Log("audiomix: CreateSourceVoice failed (0x%08X)", (unsigned)hr);
            return false;
        }
    }

    XAUDIO2_BUFFER b = {};
    b.AudioBytes = t.bytes;
    b.pAudioData = t.pcm;
    b.Flags      = XAUDIO2_END_OF_STREAM;
    b.LoopCount  = XAUDIO2_LOOP_INFINITE;   // seamless, and not our problem

    t.voice->FlushSourceBuffers();
    if (FAILED(t.voice->SubmitSourceBuffer(&b))) return false;
    t.voice->SetVolume(t.level);
    if (FAILED(t.voice->Start(0))) return false;

    t.playing = true;
    return true;
}

inline void Stop(Track& t)
{
    if (!t.voice) return;
    if (!t.playing) return;
    t.voice->Stop(0);
    t.voice->FlushSourceBuffers();
    t.playing = false;
}

inline void SetLevel(Track& t, float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    t.level = level;
    if (t.voice) t.voice->SetVolume(level);
}

inline void Release(Track& t)
{
    Stop(t);
    if (t.voice) { t.voice->DestroyVoice(); t.voice = nullptr; }
    if (t.pcm)   { free(t.pcm); t.pcm = nullptr; }
    t.bytes = 0;
}

} // namespace audiomix
