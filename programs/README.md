# Programs

## bench\_block

Benchmark suffix sorting and LZ77 factorization with variable block sizes.

### Usage

Block sizes need to be specified as log2 sizes, which guarantees that block
sizes are powers of two.

```
Usage: ./build/programs/bench_block [file] [log2_min_bs] [log2_max_bs] [target]
    file           Path to test file
    log2_min_bs    Log2 of minimum block size
    log2_max_bs    Log2 of maximum block size
    target         Benchmark target: 'kkp2/KKP2' or 'kkp3/KKP3'
```

### Example

Benchmark `KKP2` factorization for file `/path/to/file` with single block
size of 32kiB.

```
./programs/bench_block /path/to/file 15 15 kkp2
```

Benchmark `KKP3` factorization for file `/path/to/file` with block sizes
starting from 16kiB and ending to 512kiB.

```
./programs/bench_block /path/to/file 14 19 kkp3
```
