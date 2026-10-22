#include "gpu/edram.h"

#include <algorithm>
#include <cmath>

namespace edram
{
uint32_t HeightPixels(uint32_t pitchTiles32, uint32_t msaa)
{
    if (!pitchTiles32)
        return 0;
    // Rows of tiles until the base comes round again (the last row may be partial).
    uint32_t tileRows = (kTileCount + pitchTiles32 - 1) / pitchTiles32;
    tileRows = std::min(tileRows, (kMaxHeight << SamplesYLog2(msaa)) / kTileHeightSamples);
    return tileRows * (kTileHeightSamples >> SamplesYLog2(msaa));
}

namespace
{
int32_t Signed15(uint32_t v) { return int32_t(v << 17) >> 17; }
} // namespace

Rect Scissor(const PassRegisters& r)
{
    int32_t left = int32_t(r.scissorTL & 0x3FFF), top = int32_t((r.scissorTL >> 16) & 0x3FFF);
    int32_t right = int32_t(r.scissorBR & 0x3FFF), bottom = int32_t((r.scissorBR >> 16) & 0x3FFF);
    if (!(r.scissorTL >> 31)) // window_offset_disable clear
    {
        const int32_t wx = Signed15(r.windowOffset), wy = Signed15(r.windowOffset >> 16);
        left += wx; right += wx; top += wy; bottom += wy;
    }
    left = std::max(left, int32_t(r.screenScissorTL & 0x7FFF));
    top = std::max(top, int32_t((r.screenScissorTL >> 16) & 0x7FFF));
    right = std::min(right, int32_t(r.screenScissorBR & 0x7FFF));
    bottom = std::min(bottom, int32_t((r.screenScissorBR >> 16) & 0x7FFF));
    // Clamped to the pitch, and collapsed rather than reordered when inverted.
    const int32_t pitch = int32_t(r.surfaceInfo & 0x3FFF);
    left = std::clamp(left, 0, pitch);
    right = std::clamp(right, left, pitch);
    top = std::max(top, 0);
    bottom = std::max(bottom, top);
    return { left, top, right, bottom };
}

Pass DescribePass(const PassRegisters& r)
{
    Pass p;
    p.pitchPixels = r.surfaceInfo & 0x3FFF;
    p.msaa = std::min<uint32_t>((r.surfaceInfo >> 16) & 3, k4x);
    p.pitchTiles32 = PitchTiles32(p.pitchPixels, p.msaa);
    p.scissor = Scissor(r);
    const uint32_t mode = r.modeControl & 7;
    if ((mode != 4 && mode != 5) || !p.pitchTiles32)
        return p;

    // Depth/stencil: used when tested or written (stencil_enable, z_enable).
    uint32_t used = 0;
    uint32_t bases[5]{};
    if (r.depthControl & 3)
    {
        used |= 1;
        bases[0] = r.depthInfo & 0x7FF;
        p.targets[0] = { bases[0], p.pitchTiles32, p.msaa, true, (r.depthInfo >> 16) & 1 };
    }
    // Colour: in mode 4, the targets with a write mask the pixel shader writes.
    if (mode == 4)
        for (uint32_t i = 0; i < 4; i++)
        {
            if (!((r.colorMask >> (4 * i)) & 0xF) || !((r.psColorTargets >> i) & 1))
                continue;
            used |= 2u << i;
            bases[1 + i] = r.colorInfo[i] & 0x7FF;
            p.targets[1 + i] = { bases[1 + i], p.pitchTiles32, p.msaa, false, StorageFormat((r.colorInfo[i] >> 16) & 0xF) };
        }
    // One target per base: colour 0 first, depth last.
    const uint32_t order[5] = { 1, 2, 3, 4, 0 };
    for (uint32_t a = 0; a < 5; a++)
        if ((used >> order[a]) & 1)
            for (uint32_t b = a + 1; b < 5; b++)
                if (((used >> order[b]) & 1) && bases[order[b]] == bases[order[a]])
                {
                    used &= ~(1u << order[b]);
                    p.targets[order[b]] = {};
                }
    p.used = used;
    if (!used)
        return p;

    // Rows the draw can reach: the scissor, and the viewport when one is set (Direct3D 9 sets
    // the scissor from it, so it never reaches further than the viewport does).
    uint32_t maxY = uint32_t(p.scissor.bottom);
    if (r.vteCntl & 0xC) // y scale or offset enabled
    {
        float bottom = 0;
        if ((r.modeCntl >> 16) & 1) // vtx_window_offset_enable
            bottom += float(Signed15(r.windowOffset >> 16));
        if (!(r.vtxCntl & 1)) // pixel centre at integers (Direct3D 9)
            bottom += 0.5f;
        if (r.vteCntl & 8)
            bottom += r.viewportYOffset;
        bottom += (r.vteCntl & 4) ? std::fabs(r.viewportYScale) : 1.0f;
        maxY = uint32_t(std::min(float(maxY), std::max(0.0f, bottom))); // (NaN drops to 0)
    }
    p.heightUsed = std::min(maxY, HeightPixels(p.pitchTiles32, p.msaa));

    // Each target's tiles: the rows used, but not into the next target's base (MRT and
    // depth placed after each other), with 32 bpp targets counted at half the length of
    // 64 bpp ones so a 32 bpp target is not cut short by a 64 bpp neighbour's distance.
    const uint32_t lengthAt32 = ((p.heightUsed << SamplesYLog2(p.msaa)) + kTileHeightSamples - 1) / kTileHeightSamples * p.pitchTiles32;
    uint32_t sorted[5], count = 0;
    for (uint32_t i = 0; i < 5; i++)
        if ((used >> i) & 1)
            sorted[count++] = i;
    std::sort(sorted, sorted + count, [&](uint32_t a, uint32_t b) { return bases[a] != bases[b] ? bases[a] < bases[b] : a < b; });
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t i = sorted[k];
        const uint32_t next = k + 1 < count ? bases[sorted[k + 1]] : kTileCount + bases[sorted[0]];
        const uint32_t length = lengthAt32 << uint32_t(p.targets[i].Is64bpp());
        p.lengthTiles[i] = std::min(length, next - bases[i]);
    }
    return p;
}

Ownership::Ownership() { ranges_.emplace(0, Range{ kTileCount, {} }); }

void Ownership::ClaimExtent(const SurfaceKey& dest, uint32_t start, uint32_t end, std::vector<Transfer>* transfers)
{
    if (end <= start)
        return;
    // Split the range containing `start` (the map always covers 0..kTileCount).
    auto it = std::prev(ranges_.upper_bound(start));
    if (it->first < start && it->second.owner != dest)
    {
        Range tail = it->second;
        it->second.end = start;
        it = ranges_.emplace_hint(std::next(it), start, tail);
    }
    while (it != ranges_.end() && it->first < end)
    {
        if (it->second.owner == dest)
        {
            ++it;
            continue;
        }
        if (it->second.end > end)
        {
            ranges_.emplace_hint(std::next(it), end, it->second);
            it->second.end = end;
        }
        if (transfers && !it->second.owner.Empty())
        {
            if (!transfers->empty() && transfers->back().endTiles == it->first && transfers->back().source == it->second.owner)
                transfers->back().endTiles = it->second.end;
            else
                transfers->push_back({ it->first, it->second.end, it->second.owner });
        }
        it->second.owner = dest;
        // Merge with the neighbours now owned by the same layout.
        if (auto next = std::next(it); next != ranges_.end() && next->second.owner == dest)
        {
            it->second.end = next->second.end;
            ranges_.erase(next);
        }
        if (it != ranges_.begin())
            if (auto prev = std::prev(it); prev->second.owner == dest)
            {
                prev->second.end = it->second.end;
                ranges_.erase(it);
                it = prev;
            }
        ++it;
    }
}

void Ownership::Claim(const SurfaceKey& dest, uint32_t startRelative, uint32_t lengthTiles, std::vector<Transfer>* transfers)
{
    lengthTiles = std::min(lengthTiles, kTileCount);
    if (!lengthTiles)
        return;
    const uint32_t start = (dest.baseTiles + startRelative) & (kTileCount - 1), end = start + lengthTiles;
    ClaimExtent(dest, start, std::min(end, kTileCount), transfers);
    if (end > kTileCount) // into the next addressing period: wraps to tile 0
        ClaimExtent(dest, 0, std::min(end - kTileCount, start), transfers);
}

SurfaceKey Ownership::OwnerAt(uint32_t tile) const
{
    auto it = ranges_.upper_bound(tile & (kTileCount - 1));
    return std::prev(it)->second.owner;
}

std::vector<Transfer> Ownership::Ranges() const
{
    std::vector<Transfer> out;
    for (const auto& [start, range] : ranges_)
        if (!range.owner.Empty())
            out.push_back({ start, range.end, range.owner });
    return out;
}
} // namespace edram
