/*
 * Trace-driven timing model of the MRISC32-A1 pipeline.
 *
 * Reads an mr32sim debug trace (one 20-byte record per executed instruction or
 * vector element) and the static table from gen_pcinfo.py, and estimates how
 * many clock cycles the A1 core would need: operand hazards, divider stalls,
 * branch mispredictions, cache misses and write-through store traffic.
 *
 * Usage: a1time pcinfo.bin [options] < trace
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { C_ALU, C_F1, C_F2, C_MUL, C_F3, C_F4, C_DIV, C_FDIV, C_LOAD, C_STORE,
       C_BRANCH, C_JUMP, C_CALL, C_RET, C_NOP, NUM_CLASSES };
static const char *class_names[NUM_CLASSES] = {
    "alu", "f1", "sau/f2", "mul", "f3", "fadd/fmul", "div", "fdiv", "load", "store",
    "branch", "jump", "call", "ret", "nop"};
static const int latency[NUM_CLASSES] = {1, 1, 2, 3, 3, 4, 3, 4, 2, 1, 1, 1, 1, 1, 1};

#pragma pack(push, 1)
typedef struct {
    uint8_t cls, dst, dst_vec, src[3], src_vec, is_vec, size, scale, mode, pad;
    int32_t imm;
    uint32_t func;
} pcinfo_t;
#pragma pack(pop)

typedef struct {
    uint32_t flags, pc, a, b, c;
} rec_t;

enum { R_ROM, R_VRAM, R_XRAM, R_MMIO, NUM_REGIONS };
static const char *region_names[NUM_REGIONS] = {"rom", "vram", "xram", "mmio"};

static struct {
    uint32_t dsize, isize, line, vc_entries, wq_depth;
    uint32_t fill[NUM_REGIONS], write[NUM_REGIONS];
    uint32_t div_stall, fdiv_stall, mispredict, vc_penalty;
    uint64_t start, end;
    int top;
} cfg = {65536, 32768, 32, 32, 4, {8, 8, 75, 4}, {1, 1, 9, 4}, 15, 12, 3, 2, 0, UINT64_MAX, 40};

static pcinfo_t *info;
static uint32_t info_base, info_count, num_funcs;
static char **func_names;

static uint64_t t;
static uint64_t sready[32], vready[32][16];
static uint8_t sprod[32], vprod[32][16];

static uint32_t *dtag, *itag;
static uint32_t *vc_tag;
static uint32_t vc_next;
static uint64_t *wq;
static uint32_t wq_head;
static uint64_t mem_busy_until;

typedef struct {
    uint32_t tag, target;
    uint8_t valid, counter, type;
} btb_t;
static btb_t btb[512];
static uint32_t ras[16];
static uint32_t ras_top;

static struct {
    uint64_t records, cycles;
    uint64_t hazard[NUM_CLASSES];
    uint64_t div_stall, mispredicts, mispredict_cycles;
    uint64_t imiss, imiss_cycles;
    uint64_t dmiss[NUM_REGIONS], dmiss_cycles[NUM_REGIONS], vc_hits, loads, stores;
    uint64_t store_stall;
    uint64_t *func_cycles, *func_records;
    uint64_t *func_mem, *func_store, *func_fetch;
} st;

static uint64_t cur_mem, cur_store, cur_fetch;

static int region_of(uint32_t addr)
{
    switch (addr >> 30)
    {
        case 0: return R_ROM;
        case 1: return R_VRAM;
        case 2: return R_XRAM;
        default: return R_MMIO;
    }
}

static int counting(void)
{
    return st.records >= cfg.start && st.records < cfg.end;
}

static uint64_t drain_writes(uint64_t when)
{
    return mem_busy_until > when ? mem_busy_until : when;
}

static uint32_t icache_fetch(uint32_t pc)
{
    uint32_t line = pc / cfg.line;
    uint32_t idx = line % (cfg.isize / cfg.line);
    if (itag[idx] == line + 1)
    {
        return 0;
    }
    itag[idx] = line + 1;
    return cfg.fill[region_of(pc)];
}

static uint32_t dcache_load(uint32_t addr, uint64_t when)
{
    int region = region_of(addr);
    uint32_t line = addr / cfg.line;
    uint32_t idx = line % (cfg.dsize / cfg.line);
    uint64_t ready;

    if (region == R_MMIO)
    {
        return (uint32_t)(drain_writes(when) - when) + cfg.fill[R_MMIO];
    }
    if (dtag[idx] == line + 1)
    {
        return 0;
    }
    for (uint32_t i = 0; i < cfg.vc_entries; i++)
    {
        if (vc_tag[i] == line + 1)
        {
            vc_tag[i] = dtag[idx];
            dtag[idx] = line + 1;
            if (counting())
            {
                st.vc_hits++;
            }
            return cfg.vc_penalty;
        }
    }
    if (cfg.vc_entries && dtag[idx])
    {
        vc_tag[vc_next] = dtag[idx];
        vc_next = (vc_next + 1) % cfg.vc_entries;
    }
    dtag[idx] = line + 1;
    ready = drain_writes(when) + cfg.fill[region];
    mem_busy_until = ready;
    if (counting())
    {
        st.dmiss[region]++;
        st.dmiss_cycles[region] += ready - when;
    }
    return (uint32_t)(ready - when);
}

static uint32_t store(uint32_t addr, uint64_t when)
{
    int region = region_of(addr);
    uint64_t oldest = wq[wq_head];
    uint32_t stall = oldest > when ? (uint32_t)(oldest - when) : 0;
    uint64_t start = drain_writes(when + stall);

    mem_busy_until = start + cfg.write[region];
    wq[wq_head] = mem_busy_until;
    wq_head = (wq_head + 1) % cfg.wq_depth;
    return stall;
}

static uint32_t mem_address(const pcinfo_t *pi, const rec_t *r, int element)
{
    uint32_t k = pi->is_vec ? (uint32_t)element : 1;
    switch (pi->mode)
    {
        case 0: return r->a + (uint32_t)pi->imm * k;
        case 1: return r->a + r->b * pi->scale * k;
        default: return r->a + r->b * pi->scale;
    }
}

static int predict(uint32_t pc, int cls, uint32_t *target)
{
    btb_t *e = &btb[(pc >> 2) & 511];
    if (cls == C_RET)
    {
        *target = ras[(ras_top - 1) & 15];
        return 1;
    }
    if (!e->valid || e->tag != pc)
    {
        return 0;
    }
    *target = e->target;
    return cls == C_BRANCH ? e->counter >= 2 : 1;
}

static void train(uint32_t pc, int cls, int taken, uint32_t target)
{
    btb_t *e = &btb[(pc >> 2) & 511];
    if (cls == C_CALL)
    {
        ras[ras_top & 15] = pc + 4;
        ras_top++;
    }
    else if (cls == C_RET)
    {
        ras_top--;
        return;
    }
    if (taken)
    {
        if (!e->valid || e->tag != pc)
        {
            e->valid = 1;
            e->tag = pc;
            e->counter = 2;
        }
        else if (e->counter < 3)
        {
            e->counter++;
        }
        e->target = target;
        e->type = (uint8_t)cls;
    }
    else if (e->valid && e->tag == pc && e->counter > 0)
    {
        e->counter--;
    }
}

static void process(const rec_t *r, const rec_t *next, int element)
{
    const pcinfo_t *pi;
    uint64_t start = t, ready = t;
    int hazard_cls = -1;
    int count = counting();

    if (r->pc < info_base || (r->pc - info_base) / 4 >= info_count)
    {
        t++;
        return;
    }
    pi = &info[(r->pc - info_base) / 4];

    if (element == 0)
    {
        uint32_t miss = icache_fetch(r->pc);
        if (miss)
        {
            t += miss;
            cur_fetch += miss;
            if (count)
            {
                st.imiss++;
                st.imiss_cycles += miss;
            }
        }
    }

    for (int i = 0; i < 3; i++)
    {
        uint8_t s = pi->src[i];
        uint64_t rt;
        uint8_t prod;
        if (s == 255)
        {
            continue;
        }
        if (pi->src_vec & (1 << i))
        {
            rt = vready[s][element & 15];
            prod = vprod[s][element & 15];
        }
        else
        {
            if (element > 0)
            {
                continue;
            }
            rt = sready[s];
            prod = sprod[s];
        }
        if (rt > ready)
        {
            ready = rt;
            hazard_cls = prod;
        }
    }
    if (ready > t)
    {
        if (count && hazard_cls >= 0)
        {
            st.hazard[hazard_cls] += ready - t;
        }
        t = ready;
    }

    if (pi->cls == C_LOAD)
    {
        uint32_t m = dcache_load(mem_address(pi, r, element), t);
        t += m;
        cur_mem += m;
        if (count)
        {
            st.loads++;
        }
    }
    else if (pi->cls == C_STORE)
    {
        uint32_t addr = mem_address(pi, r, element);
        uint32_t s = store(addr, t);
        t += s;
        cur_store += s;
        if (count)
        {
            st.store_stall += s;
            st.stores++;
        }
    }

    uint64_t issue = t;
    t++;

    if (pi->cls == C_DIV || pi->cls == C_FDIV)
    {
        uint32_t d = pi->cls == C_DIV ? cfg.div_stall : cfg.fdiv_stall;
        t += d;
        if (count)
        {
            st.div_stall += d;
        }
    }

    if (pi->dst != 255)
    {
        if (pi->dst_vec)
        {
            vready[pi->dst][element & 15] = issue + latency[pi->cls];
            vprod[pi->dst][element & 15] = pi->cls;
        }
        else if (pi->dst != 0)
        {
            sready[pi->dst] = issue + latency[pi->cls];
            sprod[pi->dst] = pi->cls;
        }
    }

    if (pi->cls >= C_BRANCH && pi->cls <= C_RET && next)
    {
        uint32_t target = 0;
        int taken = next->pc != r->pc + 4;
        int pred = predict(r->pc, pi->cls, &target);
        if (pred != taken || (taken && target != next->pc))
        {
            t += cfg.mispredict;
            if (count)
            {
                st.mispredicts++;
                st.mispredict_cycles += cfg.mispredict;
            }
        }
        train(r->pc, pi->cls, taken, next->pc);
    }

    if (count)
    {
        st.cycles += t - start;
        st.func_cycles[pi->func] += t - start;
        st.func_records[pi->func]++;
        st.func_mem[pi->func] += cur_mem;
        st.func_store[pi->func] += cur_store;
        st.func_fetch[pi->func] += cur_fetch;
    }
    cur_mem = cur_store = cur_fetch = 0;
}

static void load_info(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t hdr[3];
    if (!f || fread(hdr, 4, 3, f) != 3)
    {
        fprintf(stderr, "can't read %s\n", path);
        exit(1);
    }
    info_base = hdr[0];
    info_count = hdr[1];
    num_funcs = hdr[2];
    func_names = calloc(num_funcs, sizeof(char *));
    for (uint32_t i = 0; i < num_funcs; i++)
    {
        uint8_t len;
        if (fread(&len, 1, 1, f) != 1)
        {
            exit(1);
        }
        func_names[i] = calloc(len + 1, 1);
        if (fread(func_names[i], 1, len, f) != len)
        {
            exit(1);
        }
    }
    info = malloc(sizeof(pcinfo_t) * info_count);
    if (fread(info, sizeof(pcinfo_t), info_count, f) != info_count)
    {
        fprintf(stderr, "short pcinfo\n");
        exit(1);
    }
    fclose(f);
}

static int cmp_func(const void *a, const void *b)
{
    uint64_t ca = st.func_cycles[*(const uint32_t *)a];
    uint64_t cb = st.func_cycles[*(const uint32_t *)b];
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

static void report(void)
{
    uint64_t ideal = 0;
    for (uint32_t i = 0; i < num_funcs; i++)
    {
        ideal += st.func_records[i];
    }
    printf("records (ideal cycles): %llu\n", (unsigned long long)ideal);
    printf("modelled cycles:        %llu  (x%.3f)\n", (unsigned long long)st.cycles,
           ideal ? (double)st.cycles / ideal : 0.0);
    printf("operand hazards:\n");
    for (int c = 0; c < NUM_CLASSES; c++)
    {
        if (st.hazard[c])
        {
            printf("  %-10s %12llu  %5.1f%%\n", class_names[c], (unsigned long long)st.hazard[c],
                   100.0 * st.hazard[c] / st.cycles);
        }
    }
    printf("divider stalls:         %llu  %5.1f%%\n", (unsigned long long)st.div_stall,
           100.0 * st.div_stall / st.cycles);
    printf("branch mispredicts:     %llu  (%llu cycles, %5.1f%%)\n",
           (unsigned long long)st.mispredicts, (unsigned long long)st.mispredict_cycles,
           100.0 * st.mispredict_cycles / st.cycles);
    printf("I$ misses:              %llu  (%llu cycles, %5.1f%%)\n", (unsigned long long)st.imiss,
           (unsigned long long)st.imiss_cycles, 100.0 * st.imiss_cycles / st.cycles);
    printf("loads %llu, stores %llu, victim hits %llu\n", (unsigned long long)st.loads,
           (unsigned long long)st.stores, (unsigned long long)st.vc_hits);
    for (int r = 0; r < NUM_REGIONS; r++)
    {
        if (st.dmiss[r])
        {
            printf("D$ misses %-5s %10llu  (%llu cycles, %5.1f%%)\n", region_names[r],
                   (unsigned long long)st.dmiss[r], (unsigned long long)st.dmiss_cycles[r],
                   100.0 * st.dmiss_cycles[r] / st.cycles);
        }
    }
    printf("store stalls:           %llu  %5.1f%%\n", (unsigned long long)st.store_stall,
           100.0 * st.store_stall / st.cycles);

    uint32_t *order = malloc(sizeof(uint32_t) * num_funcs);
    for (uint32_t i = 0; i < num_funcs; i++)
    {
        order[i] = i;
    }
    qsort(order, num_funcs, sizeof(uint32_t), cmp_func);
    printf("\n%-40s %12s %7s %12s %6s %6s %6s %6s\n", "function", "cycles", "share", "ideal", "x",
           "dmiss", "store", "fetch");
    for (int i = 0; i < cfg.top && i < (int)num_funcs; i++)
    {
        uint32_t f = order[i];
        if (!st.func_cycles[f])
        {
            break;
        }
        printf("%-40s %12llu %6.2f%% %12llu %6.2f %5.1f%% %5.1f%% %5.1f%%\n", func_names[f],
               (unsigned long long)st.func_cycles[f], 100.0 * st.func_cycles[f] / st.cycles,
               (unsigned long long)st.func_records[f],
               (double)st.func_cycles[f] / st.func_records[f],
               100.0 * st.func_mem[f] / st.func_cycles[f],
               100.0 * st.func_store[f] / st.func_cycles[f],
               100.0 * st.func_fetch[f] / st.func_cycles[f]);
    }
}

static int parse_region_list(const char *s, uint32_t *out)
{
    return sscanf(s, "%u,%u,%u,%u", &out[0], &out[1], &out[2], &out[3]) == 4;
}

int main(int argc, char **argv)
{
    static rec_t buf[1 << 16];
    rec_t prev;
    int have_prev = 0, element = 0;
    size_t n;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s pcinfo.bin [--start N] [--end N] [--dsize B] [--isize B]\n"
                        "  [--fill rom,vram,xram,mmio] [--write rom,vram,xram,mmio] [--wq N]\n"
                        "  [--vc N] [--top N] < trace\n", argv[0]);
        return 1;
    }
    load_info(argv[1]);
    fcntl(0, F_SETPIPE_SZ, 1 << 20);
    setvbuf(stdin, NULL, _IOFBF, 1 << 22);
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--start")) cfg.start = strtoull(v, NULL, 0);
        else if (!strcmp(k, "--end")) cfg.end = strtoull(v, NULL, 0);
        else if (!strcmp(k, "--dsize")) cfg.dsize = strtoul(v, NULL, 0);
        else if (!strcmp(k, "--isize")) cfg.isize = strtoul(v, NULL, 0);
        else if (!strcmp(k, "--wq")) cfg.wq_depth = strtoul(v, NULL, 0);
        else if (!strcmp(k, "--vc")) cfg.vc_entries = strtoul(v, NULL, 0);
        else if (!strcmp(k, "--top")) cfg.top = atoi(v);
        else if (!strcmp(k, "--fill")) parse_region_list(v, cfg.fill);
        else if (!strcmp(k, "--write")) parse_region_list(v, cfg.write);
        else
        {
            fprintf(stderr, "unknown option %s\n", k);
            return 1;
        }
    }

    dtag = calloc(cfg.dsize / cfg.line, 4);
    itag = calloc(cfg.isize / cfg.line, 4);
    vc_tag = calloc(cfg.vc_entries + 1, 4);
    wq = calloc(cfg.wq_depth, 8);
    st.func_cycles = calloc(num_funcs, 8);
    st.func_records = calloc(num_funcs, 8);
    st.func_mem = calloc(num_funcs, 8);
    st.func_store = calloc(num_funcs, 8);
    st.func_fetch = calloc(num_funcs, 8);

    while ((n = fread(buf, sizeof(rec_t), sizeof(buf) / sizeof(buf[0]), stdin)) > 0)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (!(buf[i].flags & 1))
            {
                continue;
            }
            if (have_prev)
            {
                process(&prev, &buf[i], element);
                st.records++;
                element = buf[i].pc == prev.pc ? element + 1 : 0;
                if (st.records >= cfg.end)
                {
                    report();
                    return 0;
                }
            }
            prev = buf[i];
            have_prev = 1;
        }
    }
    if (have_prev)
    {
        process(&prev, NULL, element);
        st.records++;
    }
    report();
    return 0;
}
