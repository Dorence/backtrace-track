#!/bin/bash

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <cpp> [extra_flags...]"
  exit 0
fi

if [[ ! -f $1 ]]; then
  echo "No such file or directory: $1"
  exit 1
else
  src_path=$1
  shift
fi

# ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
BUILD_DIR="out"

CPP_FLAGS+=" -g -ldl -rdynamic -O3 -lpthread -std=c++14 $@"
TGT_FLAGS="$TGT_FLAGS -Wall $ASAN_FLAGS"
ASM_FLAGS="$ASM_FLAGS -masm=intel -fverbose-asm"

dir=$(dirname $src_path)
src=$(basename $src_path)
src_name=${src%.*}

out="${BUILD_DIR}/${dir//\//~}_${src_name}"
mkdir -p "$BUILD_DIR"

cmd_target="g++ -o $out $CPP_FLAGS $TGT_FLAGS bttrack.cpp $src_path"
cmd_assemble="g++ -S -o $out.s $CPP_FLAGS $ASM_FLAGS $src_path"
cmd_preproc="g++ -E -o $out.i $CPP_FLAGS $src_path"

echo "> $cmd_assemble"
$cmd_assemble
if (($? != 0)); then
  echo "> $cmd_preproc"
  $cmd_preproc
else
  echo "> $cmd_target && $out"
  $cmd_target && $out
fi
