# THE ONLY TRUE SUDOKU SOLVER

Other solvers use backtracking, clever heuristics, constraint propagation, and
common sense. This one has a GPU. You know which approach is more dignified.

**Dumb Sudoku** is the one true Sudoku solver: it brute-forces candidates in
base-9 order and asks the GPU to check them, batch after batch, until truth is
found. No shortcuts. No cleverness. No candidate left behind.

The title is a joke. The brute force is not.

## The sacred method

The CPU generates consecutive batches of 32,768 candidates. The C++ AMP GPU
checks every candidate in each batch; batches are reached sequentially, and
the CPU selects the first valid result. The first candidate puts `1` in every
empty cell; after that the digits advance like an odometer from `1` through
`9`, with the last blank cell changing fastest.

To make this magnificently unreasonable process less wasteful, the GPU keeps
27 bit masks: one for each row, column, and 3×3 box. For each candidate it
assigns digits to the blank cells and checks the corresponding masks. It does
not rebuild and rescan a whole 9×9 board for every candidate. This speeds up
validation; it does **not** skip candidates, change their order, or make the
search smarter. The exhaustive search space remains `9^N` for `N` empty cells.

An empty board therefore has `9^81` candidates. The One True Solver does not
promise to finish before the heat death of the universe.

## The app

The window provides an editable 9×9 grid and four actions:

- **Reset** — restore the starting position.
- **Find solution** — begin the exhaustive GPU search.
- **Load from file** — read nine lines of nine digits; `0` or `.` means empty.
- **Save to file** — save the current grid in that format.

A loaded puzzle becomes the reset position. A found solution is displayed in
the grid. Closing the window stops an ongoing search.

## The sacred benchmarks

Measured on an NVIDIA GeForce GT 630, driver `23.21.13.8813` (`388.13`, dated
2017-10-27). Times are wall-clock measurements from pressing **Find solution**
until a result appears, including GPU initialization and transfers but not app
launch. Each run used a fresh process. Hardware benchmarks are observations,
not divine guarantees: blank-cell placement, candidate order, and GPU load
matter enormously.

All tests below start from this completed grid:

```text
123456789
456789123
789123456
234567891
567891234
891234567
345678912
678912345
912345678
```

### Sequential blanks

Remove the last N cells in row-major order, starting at the bottom-right.
These are historical measurements from before the bit-mask checker.

| Empty cells | Time | Result |
| ---: | ---: | --- |
| 1 | 0.812 s | Solved |
| 2 | 0.366 s | Solved |
| 3 | 0.352 s | Solved |
| 4 | 0.424 s | Solved |
| 5 | 0.376 s | Solved |
| 6 | 0.351 s | Solved |
| 7 | 0.463 s | Solved |
| 8 | 0.462 s | Solved |
| 9 | ≥ 20 s | Stopped at the 20-second test limit |

### Seeded random blanks

The first N cells from this fixed pseudo-random permutation (seed `20261010`)
are blanked. The sets are nested and reproducible. Coordinates are 1-based.

```text
R7C8, R5C1, R6C9, R5C9, R6C4, R2C9, R2C8, R3C6,
R7C2, R3C7, R3C1, R2C7, R8C9, R9C8, R2C1
```

Historical full-board checker results:

| Empty cells | Time | Result |
| ---: | ---: | --- |
| 1 | 0.268 s | Solved |
| 2 | 0.255 s | Solved |
| 3 | 0.352 s | Solved |
| 4 | 0.363 s | Solved |
| 5 | 0.342 s | Solved |
| 6 | 0.360 s | Solved |
| 7 | 0.359 s | Solved |
| 8 | 0.684 s | Solved |
| 9 | 3.968 s | Solved |
| 10 | ≥ 20 s | Stopped at the 20-second test limit |

### Bit-mask checker: the GPU gets a tiny bit less foolish

Release builds compared on the same GT 630 with the same seeded positions and
32,768-candidate batches. Candidate order was unchanged. Times are individual
runs; small samples and GPU load can move the numbers.

| Empty cells | Full-board checker | Bit-mask checker | Change |
| ---: | ---: | ---: | ---: |
| 9 | 3.413 s, 3.388 s | 2.374 s, 2.099 s | 34% faster on average |
| 10 | 18.75 s | 17.199 s | 8% faster in this run |

Solutions were checked for valid rows, columns, and boxes, and all clues were
preserved. The 10-cell comparison is one run per checker, not a universal
speedup claim. The earlier 10-cell benchmark table stopped trials at 20 seconds;
the 18.75-second full-board result came from a later direct A/B run.

## Build

Requirements: Windows, CMake 3.20+, Visual Studio C++ with the MSVC v143
14.42 toolset, and a GPU/driver supported by C++ AMP. Source files are UTF-8
and compiled with `/utf-8`.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" `
  -T "v143,version=14.42.34433" -A x64
cmake --build build --config Release --target DumbSudoku
```

Run `build/Release/DumbSudoku.exe`. Search progress and errors appear in the
status area.
