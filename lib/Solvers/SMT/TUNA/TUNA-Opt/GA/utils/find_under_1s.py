from utils.config import CONFIG
from utils.file_utils import get_smt_list


def main():
    # TODO: 记得改读取文件
    all_smt_relative_paths = get_smt_list(CONFIG.fast_smt_path)
    over_1s_smt_relative_paths = get_smt_list(CONFIG.z3_over_1s_path)
    # 找两个list的set的差集

    under_1s_smt_relative_paths = list(set(all_smt_relative_paths) - set(over_1s_smt_relative_paths))

    # 写入到如下文件
    # TODO: 记得改读取文件
    with open(CONFIG.z3_under_1s_path, 'a') as file:
        for under_1s_smt_relative_path in under_1s_smt_relative_paths:
            file.write(f'{under_1s_smt_relative_path}\n')


if __name__ == '__main__':
    main()