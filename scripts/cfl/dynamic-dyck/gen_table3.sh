#!/bin/bash
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lotus_root="$(cd "${script_dir}/../../.." && pwd)"
dyck_binary="${DYCK_REACH_BINARY:-${lotus_root}/build/bin/lotus-cfl-dynamic-dyck}"
benchmark_root="${DYCK_BENCHMARK_ROOT:-${lotus_root}/benchmarks/real-world/CFL/DynamicDyck}"
declare -a StringArray=("antlr"  "bloat" "chart" "eclipse" "fop" "hsqldb" "jython" "luindex" "lusearch" "pmd" "xalan")
#declare -a StringArray=("antlr") 
for filename in ${StringArray[@]} ; do
    echo -e -n "${filename}\t"
    #dotfile="benchmark/dacapo_bench/decremental/${filename}.dot"
    seqfile="${benchmark_root}/dacapo_bench/incremental/${filename}_inc.seq"
    "${dyck_binary}" 1 "${benchmark_root}/init.dot" "${seqfile}"
    "${dyck_binary}" 0 "${benchmark_root}/init.dot" "${seqfile}"
    dotfile="${benchmark_root}/dacapo_bench/decremental/${filename}.dot"
    seqfile="${benchmark_root}/dacapo_bench/decremental/${filename}_dec.seq"
    "${dyck_binary}" 1 "${dotfile}" "${seqfile}"
    "${dyck_binary}" 0 "${dotfile}" "${seqfile}"
    #input_incseq="benchmark/dacapo_bench/incremental/${filename}${numstr}.seq"
    #time ../DyckReach 1  $init_dotfile $input_seqfile
    #time ../DyckReach 0  $init_dotfile $input_seqfile
    echo " "
done

declare -a StringArray2=("btree"  "compiler"  "crypto"  "helloworld"  "mushroom"  "sample"   "startup"  "xml" "check"  "compress"  "derby"   "mpegaudio"   "parser"    "scimark"  "sunflow")
#declare -a StringArray2=("btree" "check")
for filename in ${StringArray2[@]} ; do
    echo -e -n "${filename}\t"
    seqfile="${benchmark_root}/tal/incremental/${filename}_inc.seq"
    "${dyck_binary}" 1 "${benchmark_root}/init.dot" "${seqfile}"
    "${dyck_binary}" 0 "${benchmark_root}/init.dot" "${seqfile}"
    dotfile="${benchmark_root}/tal/decremental/${filename}.dot"
    seqfile="${benchmark_root}/tal/decremental/${filename}_dec.seq"
    "${dyck_binary}" 1 "${dotfile}" "${seqfile}"
    "${dyck_binary}" 0 "${dotfile}" "${seqfile}"
    echo " "
done
