#!/bin/bash
set -e

script=$0

if [ "$#" -lt 2 ]
then
    echo "Usage: $(basename $script) [git target] [bench file(s)]"
    exit
fi

git_target=$1

script_path=$(dirname $(realpath $script))
build_path=$script_path/../build-bench
iters=1
warmup_bs=20
blocksizes=$(seq 15 27)

if [ -z "$git_target" ]
then
    echo "Provide git target (commit hash / branch name) to benchmark"
    exit
fi

if [[ ! $(sudo echo 0) ]]; then exit; fi

echo "1. Checking out test target '$git_target'"
git checkout $git_target &> /dev/null

echo "2. Creating build environment"
rm -rf $build_path
mkdir $build_path

echo "3. Running cmake"
cd $build_path
cmake -DENABLE_STATS=On .. &> /dev/null
#cmake .. &> /dev/null

echo "4. Running make"
make &> /dev/null
cd ..

shift

echo -n -e "5. Warming up for benchmarks"
for (( i=1; i<=$iters; i++ ))
do
    echo -n -e " [$i/$iters]"
    rm -f $build_path/out.salz $build_path/out.unsalz

    sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
    $build_path/programs/salz -b $warmup_bs $1 $build_path/out.salz &> /dev/null

    sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
    $build_path/programs/salz -d $build_path/out.salz $build_path/out.unsalz &> /dev/null
    echo -n -e "\b\b\b\b\b\b"
done

echo ""

echo -e "6. Benchmarking\n"
for file in $@
do
    for bs in $blocksizes
    do
        for (( i=1; i<=$iters; i++ ))
        do
            rm -f $build_path/out.salz $build_path/out.unsalz

            sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
            echo -n "filename: $(basename $file), block size: $bs, "
            $build_path/programs/salz -b $bs $file $build_path/out.salz

            sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
            echo -n "    "
            $build_path/programs/salz -d $build_path/out.salz $build_path/out.unsalz

            diff -q $file $build_path/out.unsalz
        done
        echo
    done
done

echo "6. Cleaning up"
rm -rf $build_path

echo -e "7. Exiting\n"
