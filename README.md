# SALZ - Suffix array based Lempel-Ziv data compressor

## Build

```
git clone --recurse-submodules https://github.com/akiutoslahti/salz.git
cd salz
mkdir build
cd build
cmake ..
make
```

## Programs

### bench\_block

Benchmark suffix sorting and LZ77 factorization with variable block sizes.

#### Usage

Block sizes need to be specified as log2 sizes, which guarantees that block
sizes are powers of two.

```
Usage: ./programs/bench_block [file] [log2_min_bs] [log2_max_bs] [target]
    file           Filepath
    log2_min_bs    Log2 of minimum block size
    log2_max_bs    Log2 of maximum block size
    target         Benchmark target: 'sa' or 'kkp3'
```

#### Examples

Benchmark suffix sorting for file `/path/to/file` with a single block size
of 64kB.

```
./programs/bench_block /path/to/file 16 16 sa
```

Benchmark KKP3 factorization for file `/path/to/file` with block sizes
starting from 16kB and ending to 512kB.

```
./programs/bench_block /path/to/file 14 19 kkp3
```

### bench\_block\_plt.py

Plot csv formatted output generated with bench\_block tool.

#### Usage

Script takes variable list of csv files to plot into individual figures.
Plotted figures share the path with input file, with the exception of
changing extension to `.png`.

```
Usage: programs/bench_block_plt.py [file1] [file2] ...
```

#### Examples

Plotting a single csv file.

```
./programs/bench_block_plt.py path/to/csv/file
```

Plotting a list of csv files.

```
./programs/bench_block_plt.py path/to/csv/file1 path/to/csv/file2 ...
```

Plotting all csv files inside a directory `path/to/dir/`.

```
./programs/bench_block_plt.py path/to/dir/*.csv
```
