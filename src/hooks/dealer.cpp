// Easter egg: type "whysthisdealer" in the game window and every car bounces to Niko B's "Why's This
// Dealer?" (the bouncing-Yaris meme) until the game is closed, the song looping all the while.
//
// The song isn't ours to ship: put your own copy in Documents\fm2\easter\ (any format Windows decodes: mp3,
// m4a, wav, ...; the first file there), or point FM2_DEALER_SONG at one. It's decoded once with Media
// Foundation, played on a voice of its own, and its bass drives the bounce, so the cars move with the
// music itself: a 40-150 Hz band, an envelope with a fast attack and a short release, minus its slow
// average (a held bass note doesn't keep the car up). The bounce is looked up at the voice's playback
// position each frame (FM2_DEALER_OFFSET_MS shifts it for audio/display latency, default 0).
// The car-body vertex shaders lift their output by it (renderer.cpp: ApplyDirtSeamFix, vpoint.w).
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/runtime.h"
#include "log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "xaudio2.lib")

namespace fs = std::filesystem;

namespace
{
constexpr char kCode[] = "whysthisdealer";
constexpr float kLift = 0.045f;  // clip-space lift at the strongest hit (about 2% of the screen height)
constexpr uint32_t kHopHz = 200; // bounce curve resolution (5 ms)

std::atomic<bool> g_on{ false };
std::vector<float> g_curve; // bounce 0..1 per hop
uint32_t g_rate = 0, g_frames = 0;
IXAudio2SourceVoice* g_voice = nullptr;
double g_offset = 0; // seconds

fs::path FindSong()
{
    if (const char* p = getenv("FM2_DEALER_SONG"); p && *p)
        return p;
    std::error_code ec;
    const fs::path dir = UserDir() / "easter";
    fs::create_directories(dir, ec);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec))
            return e.path();
    return {};
}

// The whole song as interleaved float stereo.
bool Decode(const fs::path& file, std::vector<float>& pcm, uint32_t& rate)
{
    if (FAILED(MFStartup(MF_VERSION)))
        return false;
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader)))
        return false;
    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    HRESULT hr = reader->SetCurrentMediaType(stream, nullptr, type);
    type->Release();
    IMFMediaType* actual = nullptr;
    if (SUCCEEDED(hr))
        hr = reader->GetCurrentMediaType(stream, &actual);
    UINT32 channels = 2;
    if (SUCCEEDED(hr))
    {
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        actual->Release();
    }
    while (SUCCEEDED(hr))
    {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
        if (!sample)
            continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
        {
            BYTE* data = nullptr;
            DWORD bytes = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &bytes)))
            {
                const float* f = reinterpret_cast<const float*>(data);
                const size_t n = bytes / sizeof(float) / channels;
                for (size_t i = 0; i < n; i++)
                {
                    pcm.push_back(f[i * channels]);
                    pcm.push_back(channels > 1 ? f[i * channels + 1] : f[i * channels]);
                }
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();
    }
    reader->Release();
    return rate && !pcm.empty();
}

// The bounce per hop, from the bass.
std::vector<float> Analyse(const std::vector<float>& pcm, uint32_t rate)
{
    const size_t frames = pcm.size() / 2, hop = std::max<size_t>(1, rate / kHopHz);
    // A 40-150 Hz band: two one-pole low-passes at 150 Hz minus one at 40 Hz.
    auto alpha = [&](double hz) { return float(1.0 - std::exp(-2.0 * 3.14159265358979 * hz / rate)); };
    const float aHi = alpha(150), aLo = alpha(40);
    float lp1 = 0, lp2 = 0, lo = 0;
    std::vector<float> energy;
    double sum = 0;
    for (size_t i = 0; i < frames; i++)
    {
        const float x = 0.5f * (pcm[i * 2] + pcm[i * 2 + 1]);
        lp1 += aHi * (x - lp1);
        lp2 += aHi * (lp1 - lp2);
        lo += aLo * (lp2 - lo);
        const float band = lp2 - lo;
        sum += double(band) * band;
        if ((i + 1) % hop == 0)
        {
            energy.push_back(float(std::sqrt(sum / hop)));
            sum = 0;
        }
    }
    // Envelope: instant attack, ~120 ms release; minus a ~0.6 s average; normalised to the loud hits.
    std::vector<float> curve(energy.size());
    const float release = std::exp(-1.0f / (0.12f * kHopHz)), slow = 1.0f / (0.6f * kHopHz);
    float env = 0, avg = 0;
    for (size_t i = 0; i < energy.size(); i++)
    {
        env = std::max(energy[i], env * release);
        avg += slow * (env - avg);
        curve[i] = std::max(0.0f, env - 0.8f * avg);
    }
    std::vector<float> sorted = curve;
    std::sort(sorted.begin(), sorted.end());
    const float top = sorted.empty() ? 1.0f : std::max(sorted[sorted.size() * 97 / 100], 1e-6f);
    for (float& c : curve)
        c = std::min(c / top, 1.0f);
    return curve;
}

void Start()
{
    const fs::path song = FindSong();
    if (song.empty())
    {
        Log("dealer: code entered, but there's no song: put your copy of \"Why's This Dealer?\" in %s",
            (UserDir() / "easter").string().c_str());
        return;
    }
    std::vector<float> pcm;
    uint32_t rate = 0;
    if (!Decode(song, pcm, rate))
    {
        Log("dealer: couldn't decode %s", song.string().c_str());
        return;
    }
    g_curve = Analyse(pcm, rate);
    g_rate = rate;
    g_frames = uint32_t(pcm.size() / 2);
    if (const char* o = getenv("FM2_DEALER_OFFSET_MS"))
        g_offset = atof(o) / 1000.0;
    // A voice of its own (an XAudio2 instance besides the game's), the song looping forever.
    static IXAudio2* xaudio = nullptr;
    static IXAudio2MasteringVoice* master = nullptr;
    static std::vector<float> keep;
    keep = std::move(pcm);
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = rate * 8;
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master)) ||
        FAILED(xaudio->CreateSourceVoice(&g_voice, &f)))
    {
        Log("dealer: no audio output");
        return;
    }
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(keep.size() * sizeof(float));
    b.pAudioData = reinterpret_cast<const BYTE*>(keep.data());
    b.LoopCount = XAUDIO2_LOOP_INFINITE;
    g_voice->SubmitSourceBuffer(&b);
    g_voice->Start();
    g_on = true;
    Log("dealer: why's this dealer? (%s, %.1f s)", song.filename().string().c_str(), double(g_frames) / rate);
}
}

// The game window's typed characters (presenter.cpp).
void DealerOnChar(wchar_t c)
{
    static size_t matched = 0;
    static std::atomic<bool> busy{ false };
    const char ch = char(towlower(c));
    matched = ch == kCode[matched] ? matched + 1 : (ch == kCode[0] ? 1 : 0);
    // Once it plays, that's it until the game closes; before that (no song yet) typing it again retries.
    if (matched == sizeof(kCode) - 1)
    {
        matched = 0;
        if (!g_on && !busy.exchange(true))
            std::thread([] {
                Start();
                busy = false;
            }).detach();
    }
}

// The car bodies' lift this frame (clip space), 0 when the egg is off.
float DealerBounce()
{
    if (!g_on.load(std::memory_order_relaxed))
        return 0;
    XAUDIO2_VOICE_STATE st{};
    g_voice->GetState(&st, 0);
    const double t = double(st.SamplesPlayed % g_frames) / g_rate + g_offset;
    const size_t i = size_t(std::max(0.0, t) * kHopHz);
    return i < g_curve.size() ? kLift * g_curve[i] : 0.0f;
}
