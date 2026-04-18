"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: 读取{SMT}_{num}_workers_{num}s.csv，统计各阶段&&原始求解时间
Creation Date: 2024/11/1
"""
import csv
import hashlib
import multiprocessing
import os.path
import subprocess
import time
from typing import Optional
from collections import defaultdict

import pandas as pd

from utils.config import CONFIG
from utils.logger import rq1_info_logger, rq1_error_logger

def read_csv_to_dict(file_path):
    """ 返回一个dict，key为file_relative_path，value为对应的llvm pass list """
    with open(file_path, mode='r', newline='') as file:
        reader = csv.DictReader(file)
        result = {}

        for row in reader:
            file_relative_path = row['file_relative_path']
            enabled_passes = [key for key, value in row.items() if key != 'file_relative_path' and key != 'cost_time' and value == '1']
            result[file_relative_path] = enabled_passes

    return result

def read_ga_label_csv_to_dict(file_path):
    """ 返回一个dict，key为file_relative_path，value为对应的求解时间四元组 list """
    # df = pd.read_csv(file_path)
    # cost_times = df['cost_time'].values
    # file_relative_paths = df['file_relative_path']
    # result = {file_relative_paths[i]: [cost_times[i], cost_times[i+1], cost_times[i+2], cost_times[i+3]]
    #         for i in range(0, len(cost_times), 4)
    #         if all(x < 429496 for x in cost_times[i:i+4])}
    # return result
    # 使用defaultdict来存储每个file_relative_path对应的cost_time列表
    path_to_times = defaultdict(list)

    with open(file_path, mode='r') as csv_file:
        csv_reader = csv.DictReader(csv_file)
        for row in csv_reader:
            file_relative_path = row['file_relative_path']
            cost_time = float(row['cost_time'])
            path_to_times[file_relative_path].append(cost_time)

    # 处理每个file_relative_path的cost_time列表
    result = {}
    for path, times in path_to_times.items():
        if len(times) >= 4:
            # 取前三个和最后一个
            selected_times = times[:3] + [times[-1]]
        else:
            # 如果不足4个，则全部取（虽然题目说可能是6个或4个，但这里做保护）
            selected_times = times
        result[path] = selected_times

    return result


class PhraseTimeCounter:
    def __init__(self, smt_relative_path: str):
        self.OPT_INF = 4294967295.0
        self.opt_path: str = os.path.join(CONFIG.llvm_dir, 'opt')
        self.slot_path: str = CONFIG.original_slot_path
        self.smt_relative_path: str = smt_relative_path
        self.smt_path: str = os.path.join(CONFIG.dataset_dir, smt_relative_path)
        self.llvm_path: str = os.path.join(CONFIG.rq1_tmp_dir, f"{smt_relative_path.replace('/', '_').replace('.', '_')}.ll")
        self.llvm_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                          f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-opt.ll")
        self.smt_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                         f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-opt.smt2")
        self.smt_to_llvm_time = self.smt_to_llvm()
        self.smt_result: Optional[str] = None

    @staticmethod
    def get_random_bytes() -> str:
        random_bytes = os.urandom(8)
        sha256_hash = hashlib.sha256(random_bytes).hexdigest()
        return sha256_hash[-4:]

    def smt_to_llvm(self) -> float:
        """ SMTlib2 to LLVM IR, return convert time """
        try:
            result = subprocess.run([
                CONFIG.smt_to_llvm_path,
                '-s', self.smt_path,
                '-lu', self.llvm_path
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
        except subprocess.TimeoutExpired:
            rq1_error_logger.error('[SMT2LLVM TIMEOUT]', CONFIG.smt_to_llvm_path,
                                   '-s', self.smt_path,
                                   '-lu', self.llvm_path)
            return float(self.OPT_INF)
        else:
            result_list = str(result.stdout).strip().split(',')
            if len(result_list) == 2:
                rq1_info_logger.info(f"[SMT2LLVM] {self.smt_relative_path}")
                return float(result_list[-1])
            else:
                rq1_error_logger.error(f"[SMT2LLVM] {self.smt_path} {result.stdout} {result.stderr}")
                return float(self.OPT_INF)

    def try_optimize(self, llvm_opt_path: str, pass_sub_list: list) -> float:
        """ try to optimize LLVM IR with given pass list, return convert time """
        passes_string = '-passes=' + ','.join(pass_sub_list)
        rq1_info_logger.info(f'[OPT] opt {passes_string} -S {self.llvm_path} -o {llvm_opt_path}')
        opt_start_time = time.time()
        # TODO: 如何错误处理？pass冲突？？？
        try:
            subprocess.run([
                self.opt_path,
                passes_string,
                '-S', self.llvm_path,
                '-o', llvm_opt_path
            ], timeout=CONFIG.TIMEOUT)
        except subprocess.TimeoutExpired:
            rq1_error_logger.error('[opt TIMEOUT]', passes_string,
                                   '-S', self.llvm_path,
                                   '-o', llvm_opt_path)
            return float(self.OPT_INF)
        else:
            opt_end_time = time.time()
            opt_time = opt_end_time - opt_start_time
            return opt_time

    def try_optimize_with_o3(self, llvm_opt_path: str) -> float:
        rq1_info_logger.info(f'[OPT] opt -O3 -S {self.llvm_path} -o {llvm_opt_path}')
        opt_start_time = time.time()
        try:
            subprocess.run([
                self.opt_path,
                '-O3',
                '-S', self.llvm_path,
                '-o', llvm_opt_path
            ], timeout=CONFIG.TIMEOUT)
        except subprocess.TimeoutExpired:
            rq1_error_logger.error('[opt TIMEOUT]', '-O3',
                                   '-S', self.llvm_path,
                                   '-o', llvm_opt_path)
            return float(self.OPT_INF)
        else:
            opt_end_time = time.time()
            opt_time = opt_end_time - opt_start_time
            return opt_time

    def llvm_to_smt(self, llvm_opt_path, smt_opt_path) -> float:
        """ LLVMIR to SMTlib2, return convert time """
        try:
            result = subprocess.run([
                CONFIG.llvm_to_smt_path,
                '-lo', llvm_opt_path,
                '-o', smt_opt_path
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
        except subprocess.TimeoutExpired:
            rq1_error_logger.error('[LLVM2SMT TIMEOUT]', CONFIG.llvm_to_smt_path,
                                    '-lo', llvm_opt_path,
                                    '-o', smt_opt_path)
            return float(self.OPT_INF)
        else:
            result_list = str(result.stdout).strip().split(',')
            if len(result_list) == 2:
                rq1_info_logger.info(f"[LLVM2SMT] {self.smt_relative_path}")
                return float(result_list[-1])
            else:
                rq1_error_logger.error(f"[LLVM2SMT] LLVM IR:{llvm_opt_path} {result.stdout} {result.stderr}")
                return float(self.OPT_INF)

    def smt_solve_time(self, smt_opt_path: str) -> float:
        if not os.path.exists(smt_opt_path):
            rq1_error_logger.error(f'[SMT] {smt_opt_path} does not exist')
            return float(self.OPT_INF)
        try:
            smt_start_time = time.time()
            result = subprocess.run([
                CONFIG.smt_solver_path,
                smt_opt_path,
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
            smt_end_time = time.time()
            smt_time = smt_end_time - smt_start_time
        except subprocess.TimeoutExpired:
            rq1_error_logger.warning('[SMT TIMEOUT]', smt_opt_path)
            return float(self.OPT_INF)
        else:
            result_stdout = str(result.stdout).strip()
            if result_stdout == 'sat' or result_stdout == 'unsat':
                self.smt_result = result_stdout
                rq1_info_logger.info(f'[SMT] solve {smt_opt_path} {result_stdout}')
                return smt_time
            else:
                rq1_error_logger.error('[SMT] stdout error', smt_opt_path)
                return float(self.OPT_INF)

    def slot_opt_time(self) -> float:
        """ Return SLOT Opt time """
        rq1_info_logger.info(f'[SLOT] slot -pall -m -s  {self.smt_path} -o {self.smt_opt_path}')
        opt_start_time = time.time()
        try:
            result = subprocess.run([
                CONFIG.original_slot_path,
                '-pall', '-m',
                '-s', self.smt_path,
                '-o', self.smt_opt_path,
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
        except subprocess.TimeoutExpired:
            rq1_error_logger.error('[SLOT TIMEOUT]',
                                   CONFIG.original_slot_path,
                                   '-s', self.smt_path,
                                   '-o', self.smt_opt_path)
            return float(self.OPT_INF)
        else:
            opt_end_time = time.time()
            opt_time = opt_end_time - opt_start_time
            return opt_time

    def get_all_phrase_time(self, use_o3: bool, pass_list: list) -> float:
        if use_o3:
            opt_time: float = self.try_optimize_with_o3(self.llvm_opt_path)
        else:
            opt_time: float = self.try_optimize(self.llvm_opt_path, pass_list)
        llvm_to_smt_time: float = self.llvm_to_smt(self.llvm_opt_path, self.smt_opt_path)
        opt_smt_solve_time: float = self.smt_solve_time(self.smt_opt_path)
        all_cost_time = self.smt_to_llvm_time + opt_time + llvm_to_smt_time + opt_smt_solve_time

        if os.path.exists(self.smt_opt_path):
            os.remove(self.smt_opt_path)
        return all_cost_time

    def get_all_slot_time(self) -> float:
        slot_opt_time: float = self.slot_opt_time()
        opt_smt_solve_time: float = self.smt_solve_time(self.smt_opt_path)
        slot_all_cost_time = slot_opt_time + opt_smt_solve_time

        if os.path.exists(self.smt_opt_path):
            os.remove(self.smt_opt_path)
        return slot_all_cost_time

    def count_phrase_time(self, pass_sub_list, output_csv_path) -> None:
        if pass_sub_list:
            llvm_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                              f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-opt.ll")
            smt_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                             f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-opt.smt2")
            # 原始约束求解时间
            origin_smt_time: float = self.smt_solve_time(self.smt_path)
            origin_smt_result = self.smt_result

            # RQ1 label 求解时间
            rq1_opt_time: float = self.try_optimize(llvm_opt_path, pass_sub_list)
            rq1_llvm_to_smt_time: float = self.llvm_to_smt(llvm_opt_path, smt_opt_path)
            rq1_opt_smt_time: float = self.smt_solve_time(smt_opt_path)
            rq1_all_time = self.smt_to_llvm_time + rq1_opt_time + rq1_llvm_to_smt_time + rq1_opt_smt_time
            # 检查 rq1 优化求解结果与原始是否一致
            if origin_smt_result is not None and origin_smt_result != self.smt_result:
                rq1_error_logger.error(f'[SLOT SMAC] Inconsistency {self.smt_relative_path}')

            # 清除 RQ1 中间文件
            if os.path.exists(smt_opt_path):
                os.remove(smt_opt_path)

            # SLOT 约束求解时间
            SLOT_PASS_LIST = ['instcombine', 'aggressive-instcombine', 'reassociate', 'sccp',
                              'dce', 'adce', 'instsimplify', 'gvn', 'instcombine', 'aggressive-instcombine']
            slot_opt_time: float = self.try_optimize(llvm_opt_path, SLOT_PASS_LIST)
            slot_llvm_to_smt_time: float = self.llvm_to_smt(llvm_opt_path, smt_opt_path)
            slot_opt_smt_time: float = self.smt_solve_time(smt_opt_path)
            slot_all_time = self.smt_to_llvm_time + slot_opt_time + slot_llvm_to_smt_time + slot_opt_smt_time
            # 检查 rq1 优化求解结果与原始是否一致
            if origin_smt_result is not None and origin_smt_result != self.smt_result:
                rq1_error_logger.error(f'[SLOT PALL] Inconsistency {self.smt_relative_path}')

            # 写文件
            with open(output_csv_path, 'a') as csv_file:
                csv_file.write(f'{self.smt_relative_path},'
                               f'{self.smt_to_llvm_time},{rq1_opt_time},{rq1_llvm_to_smt_time},{rq1_opt_smt_time},{rq1_all_time},'
                               f'{self.smt_to_llvm_time},{slot_opt_time},{slot_llvm_to_smt_time},{slot_opt_smt_time},{slot_all_time},'
                               f'{origin_smt_time}\n')

def process_task(file_relative_path: str, cost_time_list: list, output_csv_path: str):
    phrase_time_counter = PhraseTimeCounter(file_relative_path)
    o3_cost_time = phrase_time_counter.get_all_phrase_time(True, [])
    cost_time_list.append(o3_cost_time)
    with open(output_csv_path, 'a') as csv_file:
        csv_file.write(f'{file_relative_path},')
        for cur_time in cost_time_list:
            csv_file.write(f'{cur_time},')
        csv_file.write('\n')

def main():
    # CONFIG.smt_solver_path = '/home/ljc/NEW/cvc5-Linux-x86_64-static-gpl/bin/cvc5'
    CONFIG.TIMEOUT = 700

    output_csv_path = CONFIG.cvc5_ga_cost_time_path
    file_relative_path_to_cost_time_list = read_ga_label_csv_to_dict(CONFIG.cvc5_all_ga_label_path)

    # df = pd.read_csv(output_csv_path)
    # existed_file_paths = df['file_relative_path'].tolist()

    # 初始化导出 result csv
    with open(output_csv_path, 'w') as result_file:
        result_file.write('file_relative_path,default_solve_time,slot_solve_time,'
                          'ga_best_solve_time,ga_worst_solve_time,o3_solve_time,\n')

    # 多进程处理
    with multiprocessing.Pool(processes=50) as pool:
        # 使用imap_unordered并行处理任务
        tasks = [(file_relative_path, cost_time_list, output_csv_path)
                 for file_relative_path, cost_time_list in file_relative_path_to_cost_time_list.items()]
        pool.starmap(process_task, tasks)


if __name__ == '__main__':
    main()