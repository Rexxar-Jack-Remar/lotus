"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: 
Creation Date: 2024/10/31
"""
import os
import subprocess
import time
from multiprocessing import Pool
from subprocess import PIPE, TimeoutExpired
from typing import Optional

from utils.config import CONFIG
from utils.file_utils import get_smt_list


# def run_z3_with_timeout(smt_relative_path, max_time=1) -> Optional[int]:
#     """
#     运行Z3求解器并设置超时限制。
#
#     :param smt_relative_path: SMT2格式的文件路径
#     :param max_time: 最大时间阈值（秒）
#     :return: 求解时间或None（如果超时）
#     """
#     smt2_file = os.path.join(CONFIG.dataset_dir, smt_relative_path)
#     try:
#         # 启动Z3进程
#         process = subprocess.Popen(['z3', '-smt2', smt2_file],
#                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
#
#         # 等待指定时间或直到进程结束
#         start_time = time.time()
#         while True:
#             if process.poll() is not None:  # 如果进程已经结束
#                 elapsed_time = time.time() - start_time
#                 return elapsed_time
#
#             elapsed_time = time.time() - start_time
#             if elapsed_time > max_time:
#                 # 如果超过了设定的最大时间，终止进程
#                 process.terminate()
#                 return max_time
#     except Exception as e:
#         print(f"Error running Z3 for {smt2_file}: {e}")
#         return None
#
#
# def execute_command_wrapper(smt_relative_path, min_time=0.5, max_time=1):
#     result = run_z3_with_timeout(smt_relative_path, max_time)
#     if result and result > min_time:
#         with open(CONFIG.z3_all_smt_path, 'a') as file:
#             file.write(f'{smt_relative_path}\n')


# =====================================================================

def execute_z3_with_timeout(smt_relative_path, timeout=30) -> Optional[str]:
    smt2_file = os.path.join(CONFIG.dataset_dir, smt_relative_path)
    command = [
        CONFIG.smt_solver_path, smt2_file
    ]

    try:
        # 执行命令并设置超时时间
        result = subprocess.run(command, stdout=PIPE, stderr=PIPE, timeout=timeout)
        return smt_relative_path
    except TimeoutExpired:
        print(f'{smt_relative_path}')
        return None
    except Exception as e:
        print(f"Failed to process {smt_relative_path}: {e}")
        return None


def execute_command_wrapper2(smt_relative_path):
    # TODO: 记得改 TIMEOUT
    result = execute_z3_with_timeout(smt_relative_path, 600)
    if result:
        # pass
        # print(smt_relative_path)
        # TODO: 记得改输出文件
        with open(CONFIG.cvc5_600s_smt_path, 'a') as file:
            file.write(f'{smt_relative_path}\n')


def process_files(file_list, num_processes=4):
    with Pool(processes=num_processes) as pool:
        pool.map(execute_command_wrapper2, file_list)


def main():
    # TODO: 记得改读取文件
    all_smt_relative_paths = get_smt_list(CONFIG.cvc5_over_600s_smt_path)
    process_files(all_smt_relative_paths, num_processes=50)


if __name__ == '__main__':
    main()