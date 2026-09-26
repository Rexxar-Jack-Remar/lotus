#!/bin/bash
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "${script_dir}/gen_table3.sh" > table3_dyndyck.out
bash "${script_dir}/gen_table4.sh" > table4_dyndyck.out
