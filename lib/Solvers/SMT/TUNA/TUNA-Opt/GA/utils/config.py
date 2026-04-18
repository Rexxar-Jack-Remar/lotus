"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description:
Creation Date: 10/31/24
"""
import os
from pathlib import Path

import yaml

class Config:
    def __init__(self):
        current_file_path = os.path.abspath(__file__)
        current_dir = os.path.dirname(current_file_path)

        config_path = os.path.join(current_dir, '../config.yaml')
        self.config: dict = yaml.safe_load(open(config_path, 'r', encoding='utf-8'))

        self.slot_dir = self.config['slot_dir']
        self.slot_path = os.path.join(self.slot_dir, 'fastslot')
        self.original_slot_path = os.path.join(self.slot_dir, 'slot')
        self.smt_to_llvm_path = os.path.join(self.slot_dir, 'smt2llvm')
        self.llvm_to_smt_path = os.path.join(self.slot_dir, 'llvm2smt')
        self.rq1_tmp_dir = '/tmp/rq1'

        self.llvm_dir = self.config['llvm_dir']
        self.dataset_dir = self.config['dataset_dir']
        self.output_csv_path = self.config['output_csv_path']
        self.smac_log_output_directory = self.config['smac_log_output_directory']
        self.smt_solver_path = self.config['smt_solver_path']
        self.smt_category = Path(self.dataset_dir).name
        self.smt_solver_name = Path(self.smt_solver_path).name.lower()

        self.resources_path = os.path.join(current_dir, '../resources')
        self.smt_resources_path = os.path.join(self.resources_path, self.smt_category)
        # slot 翻译相对快的（小于20s）的 smtlib
        self.fast_smt_path = os.path.join(self.smt_resources_path, 'fastslot_under_20s_smt.txt')
        # z3 分类
        self.z3_category_path = os.path.join(self.smt_resources_path, 'z3_category')
        self.z3_under_1s_path = os.path.join(self.z3_category_path, 'z3_under_1s.txt')
        self.z3_over_1s_path = os.path.join(self.z3_category_path, 'z3_over_1s.txt')
        self.z3_30s_smt_path = os.path.join(self.z3_category_path, 'z3_over_1s_under_30s_smt.txt')
        self.z3_60s_smt_path = os.path.join(self.z3_category_path, 'z3_over_30s_under_60s_smt.txt')
        self.z3_120s_smt_path = os.path.join(self.z3_category_path, 'z3_over_60s_under_120s_smt.txt')
        self.z3_300s_smt_path = os.path.join(self.z3_category_path, 'z3_over_120s_under_300s_smt.txt')
        self.z3_600s_smt_path = os.path.join(self.z3_category_path, 'z3_over_300s_under_600s_smt.txt')
        # boolector 分类
        self.boolector_category_path = os.path.join(self.smt_resources_path, 'boolector_category')
        self.boolector_under_1s_path = os.path.join(self.boolector_category_path, 'boolector_under_1s.txt')
        self.boolector_over_1s_path = os.path.join(self.boolector_category_path, 'boolector_over_1s.txt')
        self.boolector_30s_smt_path = os.path.join(self.boolector_category_path, 'boolector_over_1s_under_30s_smt.txt')
        self.boolector_60s_smt_path = os.path.join(self.boolector_category_path, 'boolector_over_30s_under_60s_smt.txt')
        self.boolector_120s_smt_path = os.path.join(self.boolector_category_path, 'boolector_over_60s_under_120s_smt.txt')
        self.boolector_300s_smt_path = os.path.join(self.boolector_category_path, 'boolector_over_120s_under_300s_smt.txt')
        self.boolector_600s_smt_path = os.path.join(self.boolector_category_path, 'boolector_over_300s_under_600s_smt.txt')
        # cvc5 分类
        self.cvc5_category_path = os.path.join(self.smt_resources_path, 'cvc5_category')
        self.cvc5_under_1s_path = os.path.join(self.cvc5_category_path, 'cvc5_under_1s.txt')
        self.cvc5_over_1s_path = os.path.join(self.cvc5_category_path, 'cvc5_over_1s.txt')
        self.cvc5_30s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_1s_under_30s_smt.txt')
        self.cvc5_60s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_30s_under_60s_smt.txt')
        self.cvc5_120s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_60s_under_120s_smt.txt')
        self.cvc5_300s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_120s_under_300s_smt.txt')
        self.cvc5_600s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_300s_under_600s_smt.txt')
        self.cvc5_over_600s_smt_path = os.path.join(self.cvc5_category_path, 'cvc5_over_600s.txt')

        # SMAC 优化配置
        self.TIMEOUT = 1200
        self.WALLTIME_LIMIT = 1200

        self.smac_label_path = os.path.join(self.smt_resources_path, 'smac_label')
        # SMAC 优化产生的 label
        self.z3_1s_30s_6000s_label_path = os.path.join(self.smac_label_path, 'Z3_1s_30s_1_workers_6000s.csv')
        self.z3_30s_60s_1200s_label_path = os.path.join(self.smac_label_path, 'Z3_30s_60s_1_workers_1200s.csv')
        self.z3_60s_120s_6000s_label_path = os.path.join(self.smac_label_path, 'Z3_60s_120s_1_workers_6000s.csv')
        self.z3_120s_300s_12000s_label_path = os.path.join(self.smac_label_path, 'Z3_120s_300s_1_workers_12000s.csv')
        self.z3_300s_600s_30000s_label_path = os.path.join(self.smac_label_path, 'Z3_300s_600s_1_workers_30000s.csv')
        self.cvc5_1s_30s_6000s_label_path = os.path.join(self.smac_label_path, 'CVC5_1s_30s_1_workers_6000s.csv')
        self.cvc5_30s_60s_1200s_label_path = os.path.join(self.smac_label_path, 'CVC5_30s_60s_1_workers_1200s.csv')
        self.cvc5_60s_120s_6000s_label_path = os.path.join(self.smac_label_path, 'CVC5_60s_120s_1_workers_6000s.csv')

        # GA label
        self.ga_lable_path = os.path.join(self.smt_resources_path, 'ga_label')
        # Z3 GA label
        self.z3_ga_cost_time_path = os.path.join(self.ga_lable_path, 'Z3_cost_time.csv')
        self.z3_under_1s_ga_label_path = os.path.join(self.ga_lable_path, 'Z3_0s_1s.csv')
        self.z3_all_ga_label_path = os.path.join(self.ga_lable_path, 'Z3_1s_600s.csv')
        self.z3_all_ga_label_new_path = os.path.join(self.ga_lable_path, 'Z3_1s_600s_bak.csv')
        # CVC5 GA label
        self.cvc5_ga_cost_time_path = os.path.join(self.ga_lable_path, 'CVC5_cost_time.csv')
        self.cvc5_under_1s_ga_label_path = os.path.join(self.ga_lable_path, 'CVC5_0s_1s.csv')
        self.cvc5_all_ga_label_path = os.path.join(self.ga_lable_path, 'CVC5_1s_600s.csv')
        self.cvc5_all_ga_label_new_path = os.path.join(self.ga_lable_path, 'CVC5_1s_600s_bak.csv')
        # boolector GA label
        self.boolector_ga_cost_time_path = os.path.join(self.ga_lable_path, 'Bool_cost_time.csv')
        self.boolector_all_ga_label_path = os.path.join(self.ga_lable_path, 'Bool_1s_600s.csv')

        self.pass_path = os.path.join(self.resources_path, 'passes.txt')
        self.pass_list: list[str] = []
        with open(self.pass_path, 'r') as file:
            for line in file:
                self.pass_list.append(line.strip())



CONFIG = Config()
