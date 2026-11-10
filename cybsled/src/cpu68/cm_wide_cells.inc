/* cm_wide_cells.inc -- Cyber Sled widescreen (GitHub PR #72, TheSaltTrader): code that runs INSIDE the lifted master program
 * (gen/cm_lifted.c includes this through master/ghidra/lift_hooks.txt, so it sees the master's register file and memory helpers).
 * Edit it here; never in the generated file. */
/* WIDESCREEN (hand-added): the scenery cells of the neighbouring heading sectors. 0xBA66..0xBB58 emits the cells around the camera
 * from the list of ITS heading sector (index (heading_hi + 8) & 0xF0, plus 4 / 8 for the camera's half of the cell in x / z: tables
 * at [0x100914] cell offsets, [0x100910] positions), a visible set made for the 4:3 view. Called at 0xBB5C, before the list's
 * terminator: walks the lists of cs_wide_sectors() sectors on each side exactly as the loop walks its own -- the camera's cell, then
 * three groups each ended by 0, the same world-edge and map-bounds checks -- and appends the cells the own list does not hold, in
 * the loop's format (0, model, 0, x, y, z). Registers as the loop left them (D2/D4 the cell base, D3 the height, A2 the list, A3 the
 * map); A2 moves past what it added. 0 sectors (4:3, or ENG_WIDE_CULL=0) = the arcade's list untouched. */
static void cs_wide_cells(void)
{
    const int n = cs_wide_sectors();
    if (n <= 0) return;
    const uint32_t camx2 = (uint32_t)(MRD2(0x100B06) * 2) & 0xFFFF, camz2 = (uint32_t)((MRD2(0x100B0A) - 0x4000) * 2) & 0xFFFF;
    const uint32_t sub = ((camx2 >> 10) & 1 ? 4u : 0u) | ((camz2 >> 10) & 1 ? 8u : 0u), sec = (8 + MRD1(0x100B02)) & 0xF0;
    const uint16_t d2 = (uint16_t)RG2(0x0A), d3 = (uint16_t)RG2(0x0E), d4 = (uint16_t)RG2(0x12);
    const uint16_t camx = (uint16_t)MRD2(0x100B06);
    const uint32_t a3 = (uint32_t)RG4(0x2C), lo = MRD4(0x100B34), hi = MRD4(0x100B30);
    uint32_t a2 = (uint32_t)RG4(0x28);
    const uint32_t end = (a2 & ~0x3FFFu) + 0x4000 - 0x400;            /* the list's 16 KB buffer, room left for what follows */
    uint32_t seen[1024]; int ns = 0;
    for (int pass = 0; pass <= 2 * n; pass++) {                       /* 0: the own sector (its cells noted), then -1, +1, -2, +2 */
        const int off = pass == 0 ? 0 : (pass & 1 ? -((pass + 1) / 2) : pass / 2);
        const uint32_t idx = ((sec + (uint32_t)off * 0x10) & 0xF0) | sub;
        uint32_t a0 = MRD4(MRD4(0x100914) + idx) + 2, a1 = MRD4(MRD4(0x100910) + idx);
        int first = 1;
        for (int grp = 0; grp < 3; grp++) {
            for (int guard = 0; guard < 512; guard++) {
                int16_t cell = 0;
                if (!first) { cell = (int16_t)MRD2(a0); a0 += 2; if (!cell) break; }
                first = 0;
                const uint16_t z = (uint16_t)(MRD2(a1) + d4), x = (uint16_t)(MRD2(a1 + 2) + d2);
                a1 += 4;
                if ((int16_t)(uint16_t)(x + camx + camx - 0x4000) < 0) continue;
                const uint32_t a4 = (uint32_t)(int32_t)cell + a3;
                if (a4 < lo || a4 >= hi) continue;
                int dup = 0;
                for (int k = 0; k < ns; k++) if (seen[k] == a4) { dup = 1; break; }
                if (dup) continue;
                if (ns < 1024) seen[ns++] = a4;
                const uint32_t model = MRD2(a4);
                if (!model || pass == 0 || a2 + 12 > end) continue;
                MWR2(a2, 0); MWR2(a2 + 2, model); MWR2(a2 + 4, 0); MWR2(a2 + 6, x); MWR2(a2 + 8, d3); MWR2(a2 + 10, z);
                a2 += 12;
            }
            a1 += 4;
        }
    }
    RS4(0x28, a2);
}
