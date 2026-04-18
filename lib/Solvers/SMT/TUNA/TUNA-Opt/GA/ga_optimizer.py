"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: GA optimizer
Creation Date: 2025/1/7
"""
import os.path
import shutil

from mlopt.ga_opt import GA
from mlopt.params import Params
from phrase_time_counter import PhraseTimeCounter
from utils.config import CONFIG
from utils.logger import rq1_info_logger, rq1_error_logger


class GAOptimizer(PhraseTimeCounter):
    def __init__(self, smt_relative_path):
        super().__init__(smt_relative_path)

        self.llvm_opt_options = [f'{llvm_pass} = false (bool)' for llvm_pass in CONFIG.pass_list]
        self.origin_smt_solve_time = self.smt_solve_time(self.smt_path)

        # SLOT求解时间
        SLOT_PASS_LIST = ['instcombine', 'aggressive-instcombine', 'reassociate', 'sccp',
                          'dce', 'adce', 'instsimplify', 'gvn', 'instcombine', 'aggressive-instcombine']
        self.slot_smt_solve_time = self.get_all_phrase_time(use_o3=False, pass_list=SLOT_PASS_LIST)
        # GA 优化配置
        self.iterations = 32
        self.results: list[tuple[float, list[str]]] = []

    def run_target_tool(self, cmd_args: list) -> float:
        # 初始化文件路径
        random_bytes: str = self.get_random_bytes()
        llvm_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                          f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-{random_bytes}-opt.ll")
        smt_opt_path: str = os.path.join(CONFIG.rq1_tmp_dir,
                                         f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-{random_bytes}-opt.smt2")
        # 如果优化pass不为空
        if cmd_args:
            opt_time: float = self.try_optimize(llvm_opt_path, cmd_args)
        else:
            opt_time: float = 0.0
            shutil.copy(self.llvm_path, llvm_opt_path)
        llvm_to_smt_time: float = self.llvm_to_smt(llvm_opt_path, smt_opt_path)
        smt_solve_time: float = self.smt_solve_time(smt_opt_path)

        duration = self.smt_to_llvm_time + opt_time + llvm_to_smt_time + smt_solve_time
        rq1_info_logger.info(f'[ALL] {self.smt_relative_path} {duration}')
        if duration > 2 * CONFIG.TIMEOUT:
            return self.OPT_INF
        else:
            return duration

    def ga_optimize(self) -> bool:
        """Run Genetic algorithm to optimize the config."""
        def _ga_callback(para: Params) -> float:
            """for evaluating the fitness function"""
            try:
                passes = para.to_cmd_args()
                cost_time = self.run_target_tool(cmd_args=passes)
                self.results.append((cost_time, passes))
                return cost_time

            except Exception as ee:
                print(ee)
                return self.OPT_INF

        try:
            ga = GA(self.llvm_opt_options)
            for i in range(self.iterations):
                ga.evaluate(callback=_ga_callback)
                # 如果无限循环，直接return False
                if not ga.repopulate():
                    return False
                rq1_info_logger.info(f"finish {i}-th iteration of GA...")
                return True

        except Exception as ex:
            rq1_error_logger.error(ex)
            return False

    def ga_optimize_topk(self) -> None:
        if not self.ga_optimize():
            return
        self.results.sort(key=lambda x: x[0])
        with open(CONFIG.output_csv_path, 'a') as csv_file:
            # 写入原始求解时间
            csv_file.write(f'{self.smt_relative_path},{self.origin_smt_solve_time},')
            for _ in CONFIG.pass_list:
                csv_file.write('0,')
            csv_file.write(f'{self.smt_result}\n')

            # 写入 SLOT 求解时间
            csv_file.write(f'{self.smt_relative_path},{self.slot_smt_solve_time},')
            csv_file.write(f'1,1,0,0,1,0,0,1,0,0,1,1,0,1,0,0,0,1,1,0,0,1,0,{self.smt_result}\n')

            for result in self.results:
                if result[0] > self.origin_smt_solve_time:
                    break
                csv_file.write(f'{self.smt_relative_path},{result[0]},')
                for llvm_pass in CONFIG.pass_list:
                    if llvm_pass in result[1]:
                        value = 1
                    else:
                        value = 0
                    csv_file.write(f'{value},')
                csv_file.write(f'{self.smt_result}\n')


import multiprocessing

from utils.file_utils import init_csv_file, get_smt_list, get_valid_smt_list


def process_task(relative_path):
    ga_optimizer = GAOptimizer(relative_path)
    ga_optimizer.ga_optimize_topk()

def main():
    init_csv_file(CONFIG.output_csv_path, CONFIG.pass_list)
    CONFIG.TIMEOUT = 1200
    relative_paths = get_smt_list(CONFIG.fast_smt_path)
    print(len(relative_paths))

    # 增量GA
    # solved_paths = get_valid_smt_list(CONFIG.cvc5_test)
    # delta_paths = list(set(relative_paths) - set(solved_paths))
    # relative_paths = delta_paths
    # print(len(relative_paths))

    # for relative_path in relative_paths:
    #     ga_optimizer = GAOptimizer(relative_path)
    #     ga_optimizer.ga_optimize_topk()
    with multiprocessing.Pool(processes=40) as pool:
        pool.map(process_task, relative_paths)

if __name__ == '__main__':
    main()