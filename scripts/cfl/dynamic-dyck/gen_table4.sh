#!/bin/bash
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lotus_root="$(cd "${script_dir}/../../.." && pwd)"
dyck_binary="${DYCK_REACH_BINARY:-${lotus_root}/build/bin/lotus-cfl-dynamic-dyck}"
benchmark_root="${DYCK_BENCHMARK_ROOT:-${lotus_root}/benchmarks/real-world/CFL/DynamicDyck}"
declare -a StringArray=("antlr"  "bloat" "chart" "eclipse" "fop" "hsqldb" "jython" "luindex" "lusearch" "pmd" "xalan")
declare -a NumArray=("10" "20" "30" "40" "50")
#declare -a StringArray=("antlr") 
for filename in ${StringArray[@]} ; do
    echo -e -n "${filename}\t"
    for numstr in ${NumArray[@]} ; do
        dotfile="${benchmark_root}/dacapo_bench/mixed/${filename}_${numstr}init.dot"
        seqfile="${benchmark_root}/dacapo_bench/mixed/${filename}${numstr}.seq"
        "${dyck_binary}" 1 "${dotfile}" "${seqfile}"
        "${dyck_binary}" 0 "${dotfile}" "${seqfile}"
        #input_incseq="benchmark/dacapo_bench/incremental/${filename}${numstr}.seq"
        #time ../DyckReach 1  $init_dotfile $input_seqfile
        #time ../DyckReach 0  $init_dotfile $input_seqfile
    done
    echo " "
done

declare -a StringArray2=("btree"  "sample"   "parser"  "check"  "compiler"  "compress" "crypto"  "derby" "helloworld"   "mpegaudio"   "scimark"  "startup"  "sunflow" "xml" )         
#declare -a StringArray2=("btree" "check")
for filename in ${StringArray2[@]} ; do
    echo -e -n "${filename}\t"
    for numstr in ${NumArray[@]} ; do
        dotfile="${benchmark_root}/tal/mixed/${filename}_${numstr}init.dot"
        seqfile="${benchmark_root}/tal/mixed/${filename}${numstr}.seq"
        "${dyck_binary}" 1 "${dotfile}" "${seqfile}"
        "${dyck_binary}" 0 "${dotfile}" "${seqfile}"
        #input_incseq="benchmark/dacapo_bench/incremental/${filename}${numstr}.seq"
        #time ../DyckReach 1  $init_dotfile $input_seqfile
        #time ../DyckReach 0  $init_dotfile $input_seqfile
    done
    echo " "
done
