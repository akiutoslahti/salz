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
cmake .. &> /dev/null

echo "4. Running make"
make &> /dev/null
cd ..

echo -e "5. Running benchmarks\n"
shift
for file in $@
do
    for bs in {15..24}
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

echo "6. Cleaning up"
rm -rf $build_path

echo -e "7. Exiting\n"
