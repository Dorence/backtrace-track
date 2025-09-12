#!/bin/bash

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <cpp> [input_file]"
  exit 0
fi

if [[ ! -f $1 ]]; then
  echo "No such file or directory: $1"
  exit 1
fi

# ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
FASTMATH_FLAGS="-march=native -ffast-math -mavx2"

CPP_FLAGS+=" -rdynamic -O1 -lpthread -std=c++14 $FASTMATH_FLAGS" # -g
TGT_FLAGS="$TGT_FLAGS -Wall $ASAN_FLAGS"
ASM_FLAGS="$ASM_FLAGS -masm=intel -fverbose-asm"

BUILD_DIR="out"

src_path=$1
dir=$(dirname $1)
src=$(basename $1)
src_name=${src%.*}

out="out/${dir//\//~}_${src_name}"
mkdir -p 'out'

cmd_target="g++ -o $out $CPP_FLAGS $TGT_FLAGS bttrack.cpp $src_path"
cmd_assemble="g++ -S -o $out.s $CPP_FLAGS $ASM_FLAGS $src_path"
cmd_preproc="g++ -E -o $out.i $CPP_FLAGS $src_path"

## $2=input_file
if [[ $# -ge 2 ]]; then
  echo "> g++ -S ... && $cmd_target && $out <$2"
  $cmd_assemble && $cmd_target && $out <$2
else
  echo "> $cmd_assemble"
  $cmd_assemble
  if (($? != 0)); then
    echo "> $cmd_preproc"
    $cmd_preproc
  else
    echo "> $cmd_target && $out"
    $cmd_target && $out
  fi
fi
