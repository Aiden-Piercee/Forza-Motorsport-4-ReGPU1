// XMA decoder contexts. The title (FMOD) drives the console's hardware decoder
// through the kernel's XMA* exports, which edit a 64-byte context in guest
// memory: two input buffers of 2 KB XMA packets, an output ring of 256-byte
// blocks of 16-bit big-endian PCM, loop points. Each context gets an FFmpeg
// xma2 decoder fed one packet at a time. Like the hardware it runs ahead
// whenever there is output space; the consumer sets the pace.
//
// XMA_CONTEXT_DATA, big-endian dwords, bit fields counted from bit 0:
//   0: in0 packet count 0-11, loop count 12-19, in0 valid 20, in1 valid 21,
//      output block count 22-26, output write offset 27-31
//   1: in1 packet count 0-11, loop subframe end 14-16, skip 17-19,
//      subframe decode count 20-23, sample rate 27-28, stereo 29,
//      output valid 31
//   2: input read offset (bits) 0-25
//   3: loop start 0-25      4: loop end 0-25, current input buffer 31
//   5: in0 address  6: in1 address  7: output address  8: work buffer
//   9: output read offset 0-4
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"

namespace
{
constexpr uint32_t kContextSize = 0x40;
constexpr uint32_t kPacketBits = 2048 * 8;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kPacketBytes = 2048;

const bool g_traceXma = getenv("FM2_TRACE_XMA") != nullptr;

uint32_t Field(uint32_t ctx, int dword, int lo, int bits)
{
    return (LoadBE32(ctx + dword * 4) >> lo) & ((1u << bits) - 1);
}

void SetField(uint32_t ctx, int dword, int lo, int bits, uint32_t value)
{
    const uint32_t mask = ((1u << bits) - 1) << lo;
    const uint32_t a = ctx + dword * 4;
    StoreBE32(a, (LoadBE32(a) & ~mask) | ((value << lo) & mask));
}

std::mutex g_mutex;
std::set<uint32_t> g_enabled;
std::once_flag g_started;

struct Decoder
{
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> pcm; // decoded, interleaved, not yet in the output ring
    size_t pcmRead = 0;
    bool flush = true; // start of a stream or a jump: reset before the next packet
    bool starved = false;
    size_t packetsDecoded = 0, framesWritten = 0, framesDecoded = 0;
    // An input buffer whose packets are all decoded stays valid for the title until their audio
    // has reached the output ring (the hardware has no backlog, so the title takes "consumed"
    // to mean "delivered"); it would loop or stop a stream while the tail was still queued here.
    bool consumed[2] = {};
    bool holdsTail = false; // packets were decoded since the codec was last drained or reset
    bool seeked = false; // the title set the read offset since the last packet was decoded

    void Close()
    {
        avcodec_free_context(&av);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    bool Open(uint32_t ch, uint32_t hz)
    {
        Close();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
        if (!codec || !(av = avcodec_alloc_context3(codec)))
            return false;
        av->sample_rate = int(hz);
        av_channel_layout_default(&av->ch_layout, int(ch));
        av->block_align = kPacketBytes;
        // XMA2WAVEFORMATEX (34 bytes): one stream; FFmpeg reads the stream count from it.
        av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
        av->extradata_size = 34;
        av->extradata[0] = 1;
        if (avcodec_open2(av, codec, nullptr) < 0)
        {
            avcodec_free_context(&av);
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        channels = ch;
        rate = hz;
        return true;
    }

    // The end of a stream: the codec holds back about a packet of audio until more input
    // comes. Appends it (and silence up to a whole output block) and resets the codec.
    void Drain(size_t block)
    {
        if (!av)
            return;
        const size_t pcmBefore = pcm.size();
        avcodec_send_packet(av, nullptr);
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        if (const size_t rest = (pcm.size() - pcmRead) % block)
            pcm.resize(pcm.size() + (block - rest), 0);
        if (g_traceXma)
            Log("xma: drained %zu frames at the end of a stream", (pcm.size() - pcmBefore) / channels);
        flush = true;
        holdsTail = false;
    }

    // Decodes one 2 KB packet and appends its samples.
    void Decode(const uint8_t* data)
    {
        const bool wasFlush = flush;
        if (flush)
        {
            avcodec_flush_buffers(av);
            flush = false;
        }
        const size_t pcmBefore = pcm.size();
        packetsDecoded++;
        if (av_new_packet(packet, kPacketBytes) < 0)
            return;
        memcpy(packet->data, data, kPacketBytes);
        const int sent = avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        if (sent < 0)
        {
            flush = true;
            return;
        }
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        holdsTail = true;
        if (g_traceXma)
            Log("xma: decoded a packet: %zu frames (flush %d)", (pcm.size() - pcmBefore) / channels, int(wasFlush));
    }
};

std::map<uint32_t, Decoder> g_decoders;

// The title refilled an input buffer: whatever "consumed" was still pending for it is over.
void ClearConsumed(uint32_t ctx, int dword, int bit)
{
    if (dword != 0 || (bit != 20 && bit != 21))
        return;
    if (auto it = g_decoders.find(ctx); it != g_decoders.end())
        it->second.consumed[bit - 20] = false;
}

// The output ring has a free block: fill it from decoded samples. Returns false when it
// has nothing decoded to give.
bool WriteBlock(uint32_t ctx, Decoder& d)
{
    const size_t perBlock = kBlockBytes / 2;
    if (d.pcm.size() - d.pcmRead < perBlock)
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5), write = Field(ctx, 0, 27, 5);
    uint16_t* out = GuestPtr<uint16_t>(LoadBE32(ctx + 7 * 4) + write * kBlockBytes);
    for (size_t i = 0; i < perBlock; i++)
        out[i] = __builtin_bswap16(uint16_t(d.pcm[d.pcmRead + i]));
    d.pcmRead += perBlock;
    d.framesWritten += perBlock / std::max<uint32_t>(d.channels, 1);
    if (d.pcmRead > (1u << 16))
    {
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + ptrdiff_t(d.pcmRead));
        d.pcmRead = 0;
    }
    SetField(ctx, 0, 27, 5, (write + 1) % blocks);
    return true;
}

// One decoder step for a context: fill one output block, decoding the next input packet
// when needed. Returns false when it has nothing to do.
bool Step(uint32_t ctx)
{
    if (!Field(ctx, 1, 31, 1)) // output buffer not valid
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5);
    if (!blocks)
        return false;
    const uint32_t write = Field(ctx, 0, 27, 5), read = Field(ctx, 9, 0, 5);
    // Keep one block free so a full ring never reads as empty.
    if ((read + blocks - write - 1) % blocks == 0)
        return false;

    static const uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
    const uint32_t channels = Field(ctx, 1, 29, 1) ? 2 : 1, rate = kRates[Field(ctx, 1, 27, 2)];
    Decoder& d = g_decoders[ctx];
    if (!d.av || d.channels != channels || d.rate != rate)
        if (!d.Open(channels, rate))
            return false;
    if (WriteBlock(ctx, d))
        return true;

    // Everything decoded so far has been handed over. If no input is waiting, this is the end of
    // the stream: deliver what the codec still holds first.
    auto valid = [&](uint32_t index) { return Field(ctx, 0, 20 + index, 1) && !d.consumed[index]; };
    if (d.holdsTail && !valid(0) && !valid(1))
    {
        d.Drain(kBlockBytes / 2);
        if (WriteBlock(ctx, d))
            return true;
    }
    for (uint32_t i = 0; i < 2; i++)
        if (d.consumed[i])
        {
            d.consumed[i] = false;
            SetField(ctx, 0, 20 + i, 1, 0); // now the title may refill (or loop) it
        }

    uint32_t current = Field(ctx, 4, 31, 1);
    if (!valid(current))
    {
        current ^= 1;
        if (!valid(current))
        {
            if (g_traceXma && !d.starved)
                Log("xma: %08X starved (both input buffers consumed)", ctx);
            d.starved = true;
            return false; // starved: both input buffers consumed
        }
        d.starved = false;
        SetField(ctx, 4, 31, 1, current);
        SetField(ctx, 2, 0, 26, 32);
    }
    const uint32_t packets = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
    const uint32_t buffer = LoadBE32(ctx + (5 + current) * 4);
    const uint32_t packet = Field(ctx, 2, 0, 26) / kPacketBits;
    if (packet < packets)
    {
        if (g_traceXma && d.seeked)
        {
            const uint32_t header = LoadBE32(buffer + packet * kPacketBytes), start = Field(ctx, 2, 0, 26) % kPacketBits;
            if (start != 32 + ((header >> 11) & 0x7FFF))
                Log("xma: %08X decode starts mid-packet: packet %u of %u, offset %u in it, the packet's first frame at %u", ctx, packet, packets, start,
                    32 + ((header >> 11) & 0x7FFF));
        }
        d.seeked = false;
        d.Decode(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
    }

    // Loop: when the packet holding the loop end is done and loops remain, continue from
    // the loop start (packet-accurate).
    const uint32_t loops = Field(ctx, 0, 12, 8), loopEnd = Field(ctx, 4, 0, 26);
    if (loops && loopEnd / kPacketBits == packet)
    {
        if (loops != 255)
            SetField(ctx, 0, 12, 8, loops - 1);
        SetField(ctx, 2, 0, 26, Field(ctx, 3, 0, 26));
        d.flush = true;
        return true;
    }
    const uint32_t offset = (packet + 1) * kPacketBits + 32;
    if (packet + 1 >= packets)
    {
        d.consumed[current] = true; // (valid for the title until its audio is delivered)
        SetField(ctx, 4, 31, 1, current ^ 1);
        SetField(ctx, 2, 0, 26, 32);
    }
    else
        SetField(ctx, 2, 0, 26, offset);
    return true;
}

void DecoderLoop()
{
    SetThreadDescription(GetCurrentThread(), L"xma decoder");
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::milliseconds(2);
        std::this_thread::sleep_until(next);
        std::lock_guard lock(g_mutex);
        for (uint32_t ctx : g_enabled)
            while (Step(ctx))
                ;
    }
}
}

GUEST_EXPORT(XMACreateContext)
{
    const uint32_t context = GuestAlloc(Heap::Physical, kContextSize, 0x100);
    StoreIfNonNull(ctx.r3.u32, context);
    std::call_once(g_started, [] {
        av_log_set_level(AV_LOG_ERROR);
        std::thread(DecoderLoop).detach();
    });
    Return(ctx, context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

GUEST_EXPORT(XMAReleaseContext)
{
    {
        std::lock_guard lock(g_mutex);
        g_enabled.erase(ctx.r3.u32);
        if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
        {
            it->second.Close();
            g_decoders.erase(it);
        }
    }
    GuestFree(ctx.r3.u32);
    Return(ctx, 0);
}

GUEST_EXPORT(XMAInitializeContext)
{
    // XMA_CONTEXT_INIT: in0, in0 packets, in1, in1 packets, read offset,
    // output, output blocks, work buffer, subframes, channels, sample rate,
    // loop start, loop end, loop count/subframe end/skip (bytes).
    const uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    std::lock_guard lock(g_mutex);
    memset(GuestPtr(c), 0, kContextSize);
    auto in = [&](uint32_t off) { return LoadBE32(init + off); };
    StoreBE32(c + 5 * 4, in(0x00));
    SetField(c, 0, 0, 12, in(0x04));
    StoreBE32(c + 6 * 4, in(0x08));
    SetField(c, 1, 0, 12, in(0x0C));
    SetField(c, 2, 0, 26, in(0x10));
    StoreBE32(c + 7 * 4, in(0x14));
    SetField(c, 0, 22, 5, in(0x18));
    StoreBE32(c + 8 * 4, in(0x1C));
    SetField(c, 1, 20, 4, in(0x20));
    SetField(c, 1, 29, 1, in(0x24) >= 1);
    SetField(c, 1, 27, 2, in(0x28));
    SetField(c, 3, 0, 26, in(0x2C));
    SetField(c, 4, 0, 26, in(0x30));
    SetField(c, 0, 12, 8, *GuestPtr<uint8_t>(init + 0x34));
    if (g_traceXma)
        Log("xma: init %08X in0 %08X x%u in1 %08X x%u offset %u out %08X blocks %u ch %u rate %u loop %u-%u x%u", c, in(0x00), in(0x04), in(0x08), in(0x0C),
            in(0x10), in(0x14), in(0x18), in(0x24), in(0x28), in(0x2C), in(0x30), *GuestPtr<uint8_t>(init + 0x34));
    if (auto it = g_decoders.find(c); it != g_decoders.end())
    {
        if (g_traceXma)
            Log("xma: re-init %08X: %zu packets decoded, %zu frames decoded, %zu delivered, %zu discarded unread", c, it->second.packetsDecoded,
                it->second.framesDecoded, it->second.framesWritten, (it->second.pcm.size() - it->second.pcmRead) / std::max<uint32_t>(it->second.channels, 1));
        it->second.packetsDecoded = it->second.framesDecoded = it->second.framesWritten = 0;
        it->second.consumed[0] = it->second.consumed[1] = false;
        it->second.holdsTail = false;
        it->second.flush = true;
        it->second.pcm.clear();
        it->second.pcmRead = 0;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMAEnableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: enable %08X (read offset %u, in0 %u in1 %u valid, current %u) loops %u start %u end %u, subframes: end %u skip %u decode %u, packets %u/%u",
            ctx.r3.u32, Field(ctx.r3.u32, 2, 0, 26), Field(ctx.r3.u32, 0, 20, 1), Field(ctx.r3.u32, 0, 21, 1), Field(ctx.r3.u32, 4, 31, 1),
            Field(ctx.r3.u32, 0, 12, 8), Field(ctx.r3.u32, 3, 0, 26), Field(ctx.r3.u32, 4, 0, 26), Field(ctx.r3.u32, 1, 14, 3), Field(ctx.r3.u32, 1, 17, 3),
            Field(ctx.r3.u32, 1, 20, 4), Field(ctx.r3.u32, 0, 0, 12), Field(ctx.r3.u32, 1, 0, 12));
    g_enabled.insert(ctx.r3.u32);
    while (Step(ctx.r3.u32))
        ;
    Return(ctx, 0);
}

GUEST_EXPORT(XMADisableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: disable %08X", ctx.r3.u32);
    g_enabled.erase(ctx.r3.u32);
    Return(ctx, 0);
}

#define XMA_FIELD_EXPORTS(Name, dword, lo, bits)                     \
    GUEST_EXPORT(XMAGet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, lo, bits));             \
    }                                                                \
    GUEST_EXPORT(XMASet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        SetField(ctx.r3.u32, dword, lo, bits, ctx.r4.u32);           \
        Return(ctx, 0);                                              \
    }

GUEST_EXPORT(XMAGetInputBufferReadOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 2, 0, 26));
}

GUEST_EXPORT(XMASetInputBufferReadOffset)
{
    // A seek: the decoder starts over at the new packet.
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: seek %08X to offset %u (was %u), in0 %08X x%u in1 %08X x%u, out %08X blocks %u write %u read %u, skip %u", ctx.r3.u32, ctx.r4.u32,
            Field(ctx.r3.u32, 2, 0, 26), LoadBE32(ctx.r3.u32 + 20), Field(ctx.r3.u32, 0, 0, 12), LoadBE32(ctx.r3.u32 + 24), Field(ctx.r3.u32, 1, 0, 12),
            LoadBE32(ctx.r3.u32 + 28), Field(ctx.r3.u32, 0, 22, 5), Field(ctx.r3.u32, 0, 27, 5), Field(ctx.r3.u32, 9, 0, 5), Field(ctx.r3.u32, 1, 17, 3));
    SetField(ctx.r3.u32, 2, 0, 26, ctx.r4.u32);
    // The title restarts the decoder at the first frame of each next pair of packets (a seek
    // within a stream it reads in order). The codec keeps its overlap and its unfinished frame
    // across that, as the hardware does; resetting it dropped most of every pair's audio.
    static LiveSwitch flushOnSeek("FM2_XMA_SEEK_FLUSH");
    if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
    {
        it->second.seeked = true;
        if (flushOnSeek)
            it->second.flush = true;
    }
    Return(ctx, 0);
}
XMA_FIELD_EXPORTS(OutputBufferReadOffset, 9, 0, 5)

GUEST_EXPORT(XMAGetOutputBufferWriteOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 0, 27, 5));
}

#define XMA_FLAG_EXPORTS(Name, dword, bit)                           \
    GUEST_EXPORT(XMAIs##Name##Valid)                                 \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, bit, 1));               \
    }                                                                \
    GUEST_EXPORT(XMASet##Name##Valid)                                \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        if (g_traceXma)                                                          Log("xma: %08X " #Name " valid", ctx.r3.u32);                    SetField(ctx.r3.u32, dword, bit, 1, 1);                              ClearConsumed(ctx.r3.u32, dword, bit);                       \
        Return(ctx, 0);                                              \
    }

XMA_FLAG_EXPORTS(InputBuffer0, 0, 20)
XMA_FLAG_EXPORTS(InputBuffer1, 0, 21)
XMA_FLAG_EXPORTS(OutputBuffer, 1, 31)
