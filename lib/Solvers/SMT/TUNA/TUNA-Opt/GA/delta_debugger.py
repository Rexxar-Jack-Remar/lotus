"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: 剪枝原始GA产生的
Creation Date: 2024/11/1
"""
import multiprocessing
import os.path
import subprocess
import time
from typing import Optional

import pandas as pd

from utils.config import CONFIG
from utils.logger import rq1_info_logger, rq1_error_logger
from utils.file_utils import init_csv_file

def read_best_ga_label_from_csv(file_path: str) -> dict:
    """ 返回一个dict，key 为 file_relative_path，values 最优 pass 集合 """
    df = pd.read_csv(file_path)
    result_dict = {}

    # 按file_relative_path分组
    for path, group_df in df.groupby('file_relative_path'):
        # 跳过元素数量小于2的组
        if len(group_df) < 2:
            continue

        # 获取每组中的第三个元素，前两个为原始和SLOT
        second_element = group_df.iloc[2]

        # 提取值为1的编译选项
        compile_options = set()
        for column in second_element.index:
            # 排除file_relative_path列
            if column != 'file_relative_path' and second_element[column] == 1:
                compile_options.add(column)

        # 添加到结果字典中
        result_dict[path] = compile_options

    return result_dict


class DeltaDebuggger:
    def __init__(self, smt_relative_path: str, ga_opt_passes: list):
        self.OPT_INF = 4294967295.0
        self.opt_path: str = os.path.join(CONFIG.llvm_dir, 'opt')
        self.ga_opt_passes = ga_opt_passes

        self.smt_relative_path: str = smt_relative_path
        self.smt_path: str = os.path.join(CONFIG.dataset_dir, smt_relative_path)
        self.llvm_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                           f"{smt_relative_path.replace('/', '_').replace('.', '_')}.ll")
        self.llvm_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                          f"{smt_relative_path.replace('/', '_').replace('.', '_')}-opt.ll")
        self.llvm_opt_new_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                          f"{smt_relative_path.replace('/', '_').replace('.', '_')}-opt-new.ll")

    def smt_to_llvm(self) -> int:
        """ SMTlib2 to LLVM IR, return status int """
        try:
            result = subprocess.run([
                CONFIG.smt_to_llvm_path,
                '-s', self.smt_path,
                '-lu', self.llvm_path
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
            return 1
        except Exception:
            rq1_error_logger.error('[SMT2LLVM]', CONFIG.smt_to_llvm_path,
                                   '-s', self.smt_path,
                                   '-lu', self.llvm_path)
            return -1

    def try_optimize(self, llvm_opt_path: str, pass_sub_list: list) -> int:
        """ try to optimize LLVM IR with given pass list, return status int """
        passes_string = '-passes=' + ','.join(pass_sub_list)
        rq1_info_logger.info(f'[OPT] opt {passes_string} -S {self.llvm_path} -o {llvm_opt_path}')
        # TODO: 如何错误处理？pass冲突？？？
        try:
            subprocess.run([
                self.opt_path,
                passes_string,
                '-S', self.llvm_path,
                '-o', llvm_opt_path
            ], timeout=CONFIG.TIMEOUT)
            return 1
        except Exception:
            rq1_error_logger.error('[opt]', passes_string,
                                   '-S', self.llvm_path,
                                   '-o', llvm_opt_path)
            return -1

    def refine_opt_passes(self) -> list:
        """使用迭代方法找到保持输出一致的最小opt_passes集合"""
        # 首先转换SMT到LLVM IR
        if self.smt_to_llvm() != 1:
            rq1_error_logger.error("Failed to convert SMT to LLVM IR")
            return []

        # 用原始完整的pass列表生成参考输出
        if self.try_optimize(self.llvm_opt_path, self.ga_opt_passes) != 1:
            rq1_error_logger.error("Failed to optimize with original pass list")
            return []

        # 读取参考输出文件内容
        try:
            with open(self.llvm_opt_path, 'r', encoding='utf-8') as f:
                reference_output = f.read()
        except Exception as e:
            rq1_error_logger.error(f"Failed to read reference output: {e}")
            return []

        def test_pass_list(pass_list: list) -> bool:
            """测试给定pass列表是否能产生与参考输出一致的结果"""
            if not pass_list:  # 空列表，比较原始文件
                try:
                    with open(self.llvm_path, 'r', encoding='utf-8') as f:
                        current_output = f.read()
                except Exception:
                    return False
            else:
                if self.try_optimize(self.llvm_opt_new_path, pass_list) != 1:
                    return False
                try:
                    with open(self.llvm_opt_new_path, 'r', encoding='utf-8') as f:
                        current_output = f.read()
                except Exception:
                    return False

            return current_output == reference_output

        # 开始迭代剪枝
        current_passes = list(self.ga_opt_passes.copy())
        rq1_info_logger.info(f"Starting iterative pruning with {len(current_passes)} passes")

        changed = True
        iteration = 0

        while changed:
            changed = False
            iteration += 1
            rq1_info_logger.info(f"Iteration {iteration}: testing {len(current_passes)} passes")

            # 尝试移除每个pass
            for i in range(len(current_passes)):
                # 创建移除第i个pass的新列表
                test_passes = current_passes[:i] + current_passes[i + 1:]

                rq1_info_logger.info(f"  Testing removal of pass '{current_passes[i]}' ({i + 1}/{len(current_passes)})")

                # 测试移除这个pass后是否仍能产生相同输出
                if test_pass_list(test_passes):
                    rq1_info_logger.info(f"  Successfully removed pass '{current_passes[i]}'")
                    current_passes = test_passes
                    changed = True
                    break  # 移除一个pass后重新开始遍历
                else:
                    rq1_info_logger.info(f"  Pass '{current_passes[i]}' is necessary, keeping it")

            if not changed:
                rq1_info_logger.info(f"No more passes can be removed in iteration {iteration}")

        # 最终验证
        if not test_pass_list(current_passes):
            rq1_error_logger.error("Final pass list doesn't produce expected output, returning original")
            return self.ga_opt_passes

        rq1_info_logger.info(
            f"Pruning complete: reduced from {len(self.ga_opt_passes)} to {len(current_passes)} passes")
        rq1_info_logger.info(f"Final minimal pass list: {current_passes}")

        return current_passes


def process_task(file_relative_path: str, ga_opt_passes: list, output_csv_path: str):
    delta_debugger = DeltaDebuggger(file_relative_path, ga_opt_passes)
    refine_passes = delta_debugger.refine_opt_passes()

    with open(output_csv_path, 'a') as csv_file:
        csv_file.write(f'{file_relative_path},114514,')
        for llvm_pass in CONFIG.pass_list:
            if llvm_pass in refine_passes:
                value = 1
            else:
                value = 0
            csv_file.write(f'{value},')
        csv_file.write('\n')


def main():
    base_name = 'Z3_QF_FP_1s_120s'
    ga_label_path = f'/home/ljc/NEW/COS-for-SMT/code/RQ1/resources/QF_BV/new_ga_label/{base_name}.csv'
    refine_ga_label_path = f'/home/ljc/NEW/COS-for-SMT/code/RQ1/resources/QF_BV/new_ga_label/Refine_{base_name}.csv'
    file_relative_path_to_ga_opt_passes = read_best_ga_label_from_csv(ga_label_path)

    init_csv_file(refine_ga_label_path, CONFIG.pass_list)

    # for file_relative_path, ga_opt_passes in file_relative_path_to_ga_opt_passes.items():
    #     delta_debugger = DeltaDebuggger(file_relative_path, ga_opt_passes)
    #     refine_passes = delta_debugger.refine_opt_passes()
    #
    #     with open(refine_ga_label_path, 'a') as csv_file:
    #         csv_file.write(f'{file_relative_path},')
    #         for llvm_pass in CONFIG.pass_list:
    #             if llvm_pass in refine_passes:
    #                 value = 1
    #             else:
    #                 value = 0
    #             csv_file.write(f'{value},')
    #         csv_file.write('\n')

    # 多进程处理
    with multiprocessing.Pool(processes=40) as pool:
        # 使用imap_unordered并行处理任务
        tasks = [(file_relative_path, ga_opt_passes, refine_ga_label_path)
                 for file_relative_path, ga_opt_passes in file_relative_path_to_ga_opt_passes.items()]
        pool.starmap(process_task, tasks)


if __name__ == '__main__':
    main()