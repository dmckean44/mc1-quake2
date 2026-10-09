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

* `--low xram` time addresses below 0x40000000 as SDRAM (for programs built
  for the simulator without the MC1 SDK, which run from low memory)

Defaults approximate today's MC1 (16-bit SDRAM, one row cycle per 32-bit
access). `--fill 8,8,24,4 --write 1,1,2,4 --wq 8` approximates the planned
MiSTer memory path with a write-back cache in front of SDRAM.
