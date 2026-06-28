## Seal

Building symbolic automata for stateful systems

## Disclaimer

We were informed that the full artifact cannot be released due to intellectual property 
restrictions. Recognizing the importance of transparency and reproducibility, 
we tried best to work with the agency to obtain permission to release an academic version.

Since this academic version is prepared in a rush and some key components are replaced by public ones.
There may be a lot of rough edges and bugs.

Nonetheless, we include a few runnable examples in the artifact for demonstration.

## Build & Run

Compiler
* gcc-11.2.0

Dependency: 
* llvm-12.0.1
* z3-4.8.12, better to use the copy included. Please use cmake to build and install this z3.

Use the command below to build Seal
```bash
$ mkdir build
$ cd build
$ cmake ..
$ make
```

Use the command below to run Seal over example programs

```bash
make regression
[ 27%] Built target PPYTransform
[ 52%] Built target PPYCore
[ 68%] Built target PPYMemory
[ 93%] Built target PPYSupport
[100%] Built target seal
[INFO] ----------------------------------------------------
[INFO] Regression begins
[INFO] ----------------------------------------------------
[INFO] Running on mode_autorotate with popeye_main_mode_autorotate ...Pass! 1 seconds. Done!
[INFO] Running on mode_flip with popeye_main_mode_flip ...Pass! 0 seconds. Done!
[INFO] Running on mode_planertl with popeye_main_mode_planertl ...Pass! 0 seconds. Done!
[INFO] Running on mode_roversmartrtl with popeye_main_mode_roversmartrtl ...Pass! 1 seconds. Done!
[INFO] Running on mode_smartrtl with popeye_main_mode_smartrtl ...Pass! 2 seconds. Done!
[INFO] Running on mode_subcontrolmotordetect with popeye_main_mode_subcontrolmotordetect ...Pass! 0 seconds. Done!
[INFO] Running on mode_takeoff with popeye_main_mode_takeoff ...Pass! 0 seconds. Done!
[INFO] Running on mode_throw with popeye_main_mode_throw ...Pass! 0 seconds. Done!
[INFO] ----------------------------------------------------
[INFO] Regression completes
[INFO] ----------------------------------------------------
[100%] Built target regression
```

The automata are output to `/build/benchmarks/*.{dot,png}`.
