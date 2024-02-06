# MC1 Quake II

This is a port of id Software's game Quake II for the MRISC32 based MC1 computer.

This code is a fork of Yamagi Quake II 8.31pre, which itself is built upon Icculus Quake II, which
itself is based on Quake II 3.21.

See the LICENSE file for license information.

## Changes

* Supports static linking of game and rendering libraries (renderer is selected at compile time).
* Can be built for the [MC1 computer](https://gitlab.com/mrisc32/mc1).

## Building

### For a MC1 computer

Prerequisites:

* [mrisc32-gnu-toolchain](https://gitlab.com/mrisc32/mrisc32-gnu-toolchain) installed in your `PATH`.
* [MC1 SDK](https://gitlab.com/mrisc32/mc1-sdk) installed in `$HOME/mc1-sdk` (default installation).

```bash
mkdir out && cd out
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../mc1-toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
ninja
```

### For the MRISC32 simulator

Prerequisites:

* Same as for the MC1 computer, plus...
* The [MRISC32 Simulator](https://gitlab.com/mrisc32/mrisc32-simulator) (for running Quake II).

```bash
mkdir out && cd out
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=../mc1-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DMR32SIM=TRUE ..
ninja
```

### For your host machine (e.g. Linux)

Prerequisites:

* [SDL2](https://www.libsdl.org/) development libraries.

```bash
mkdir out && cd out
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
```

## Game files

To run MC1 Quake II, you need Quake II game files. The easiest is to download the Quake II Demo.

MC1 Quake II needs the contents of the `baseq2` folder of an installation (especially the
`pak0.pak` file). You can specify the location of the gamefiles with the `-datadir` command line
option (give the path to the folder that contains baseq2).

E.g. search the internet for "q2-314-demo" or try this:

* [q2-314-demo-x86.exe](https://www.boxedwine.org/wp-content/uploads/2018/02/q2-314-demo-x86.exe) (boxedwine.org)
  - unzip the exe-file and copy the files under `Install/Data`
