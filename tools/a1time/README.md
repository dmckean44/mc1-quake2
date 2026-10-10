# a1time

Trace-driven timing model of the MRISC32-A1 core. `mr32sim` only counts ideal
cycles; this replays its debug trace through a model of the A1 pipeline
(operand forwarding latencies, divider stalls, the BTB/RAS branch predictor,
direct-mapped I$/D$ with victim cache, write-through store queue) to estimate
real clock cycles.

    gcc -O2 -o a1time a1time.c
    python3 gen_pcinfo.py quake2 pcinfo.bin
    mkfifo trace.fifo
    ./a1time pcinfo.bin --start <cycle> --end <cycle> < trace.fifo &
    mr32sim -t trace.fifo -c <cycle> quake2 ...

Memory timing is given per region (rom, vram, xram, mmio):

* `--fill` cycles to fill a 32-byte D$/I$ line
* `--write` cycles the memory port is busy per store
* `--wq` store queue depth
* `--fill-gap` cycles the D$ stays busy after the critical word of a line fill
  arrives; loads and stores in that window wait

`--fill`, `--write` and `--fill-gap` also take a single value for all regions.
Consecutive stores to the same 64-bit word while the first is still queued
merge into one write, as the A1 does.

* `--low xram` time addresses below 0x40000000 as SDRAM (for programs built
  for the simulator without the MC1 SDK, which run from low memory)

Defaults match today's MC1 SDRAM controller (xram_sdram.vhd) at 96 MHz,
measured with mc1-mister's sim/mc1-sys: 11 cycles to the critical word, 24
more for the rest of the line, 9 cycles per 64-bit write, 8-deep write queue.
The kernels there agree within 2.5%. `--fill 8,8,24,4 --write 1,1,2,4 --wq 8
--fill-gap 3` approximates the planned MiSTer memory path with a write-back
cache in front of SDRAM.

The pipeline model is checked against the A1 RTL with mc1-mister's
sim/a1time-check (Verilator): with `--fill 6 --write 1 --fill-gap 3 --wq 4`, which
matches that test bench's memory, test kernels agree within 1-2%.
