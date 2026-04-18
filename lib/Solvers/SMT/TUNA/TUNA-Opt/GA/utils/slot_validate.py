"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description:
Creation Date: 2024/10/31
"""
import os.path
import random
import subprocess
from multiprocessing import Pool
from subprocess import PIPE, TimeoutExpired
from typing import Optional

from utils.config import CONFIG
from utils.file_utils import list_smt_files


def execute_command_with_timeout(file_relative_path: str, timeout=20) -> Optional[str]:
    file_path: str = os.path.join(CONFIG.dataset_dir, file_relative_path)
    file_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir, f"{file_relative_path.replace('/', '_').replace('.', '_')}-opt.smt2")
    command = [
        CONFIG.slot_path,
        "-m",
        "-s", f"{file_path}",
        "-o", f"{file_opt_path}",
        "-p", CONFIG.pass_path
    ]

    try:
        # 执行命令并设置超时时间
        result = subprocess.run(command, stdout=PIPE, stderr=PIPE, timeout=timeout)
        return file_relative_path
    except TimeoutExpired:
        return None
    except Exception as e:
        print(f"Failed to process {file_path}: {e}")
        return None

def execute_command_wrapper(file_relative_path: str, timeout=20):
    result = execute_command_with_timeout(file_relative_path, timeout)
    if result:
        with open(CONFIG.fast_smt_path, 'a') as file:
            file.write(f'{file_relative_path}\n')


def process_files(file_list, num_processes=4):
    with Pool(processes=num_processes) as pool:
        pool.map(execute_command_wrapper, file_list)

def main():
    relative_paths = list_smt_files(CONFIG.dataset_dir)
    random.shuffle(relative_paths)
    process_files(relative_paths, num_processes=64)

if __name__ == '__main__':
    main()