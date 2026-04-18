"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: 
Creation Date: 10/31/24
"""
import os.path
import shutil
from pathlib import Path

import pandas as pd

from utils.config import CONFIG


def file_path_to_bak_path(file_path: str) -> str:
    directory = os.path.dirname(file_path)
    base_name, ext = os.path.splitext(os.path.basename(file_path))
    backup_base_name = f"{base_name}_bak{ext}"
    backup_path = os.path.join(directory, backup_base_name)
    return backup_path

def list_smt_files(directory) -> list[str]:
    """ 返回给定路径下所有的 smt 文件的相对路径 """
    base_path = Path(directory)
    file_list = [str(file.relative_to(base_path)) for file in base_path.rglob('*.smt2') if file.is_file()]
    return file_list

def get_valid_smt_list(label_path: str) -> list:
    df = pd.read_csv(label_path)
    return df['file_relative_path'].values

def get_smt_list(smt_path: str) -> list[str]:
    """ 读取预处理过的smtlib列表 """
    fast_smt_list: list[str] = []
    with open(smt_path, 'r') as file:
        for line in file:
            fast_smt_list.append(line.strip())
    return fast_smt_list

def init_csv_file(file_path: str, pass_list: list[str]):
    """ 初始化csv第一行 """
    if os.path.exists(file_path):
        shutil.move(file_path, file_path_to_bak_path(file_path))

    with open(file_path, 'w') as csv_file:
        csv_file.write('file_relative_path,cost_time,')
        for llvm_pass in pass_list:
            csv_file.write(f'{llvm_pass},')
        csv_file.write('status\n')

def init_smac_dir():
    if os.path.exists(CONFIG.smac_log_output_directory):
        shutil.rmtree(CONFIG.smac_log_output_directory)
