import pandas as pd
import sys
import os

from numpy.distutils.conv_template import header


def process_file(file_path):
    """
    处理文本文件，按照指定要求进行数据清理和重排
    """
    try:
        # 读取文件
        with open(file_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()

        if not lines:
            print(f"文件 {file_path} 为空")
            return

        # 检查第一行是否以"op_"开头，如果是则删除
        if lines[0].strip().startswith('op_'):
            lines = lines[1:]
            print("删除了以'op_'开头的第一行")

        if not lines:
            print("删除第一行后文件为空")
            return

        # 将处理后的行写入临时文件，然后用pandas读取
        temp_content = ''.join(lines)

        # 尝试不同的分隔符
        separators = ['\t', ',', ' ', ';']
        df = None

        for sep in separators:
            try:
                # 使用StringIO来从字符串读取
                from io import StringIO
                df = pd.read_csv(StringIO(temp_content), sep=sep, dtype=str, header=None)
                if df.shape[1] > 1:  # 如果成功分割成多列
                    break
            except:
                continue

        if df is None or df.shape[1] <= 1:
            print(f"无法正确解析文件 {file_path}")
            return

        print(f"原始数据形状: {df.shape}")
        print(f"列名: {list(df.columns)}")

        # 删除全是0.0和NA的列
        cols_to_keep = []
        for col in df.columns:
            # 获取该列的所有值（除了列名）
            values = df[col].values
            # 检查是否全是0.0、'0.0'、NA、'NA'、空值
            unique_values = set(str(v).strip() for v in values if pd.notna(v) and str(v).strip() != '')

            # 如果列中只包含 '0.0', 'NA', 或为空，则删除
            if unique_values.issubset({'0.0', '0', 'NA', 'na', 'N/A'}):
                print(f"删除列 '{col}': 只包含 {unique_values}")
            else:
                cols_to_keep.append(col)

        df = df[cols_to_keep]
        print(f"删除空列后的形状: {df.shape}")

        if df.empty:
            print("删除空列后数据为空")
            return

        # 找到以.smt2结尾的列（除了列名本身以.smt2结尾的情况）
        smt2_cols = []
        other_cols = []

        for col in df.columns:
            # 检查列中的数据（不是列名）是否有以.smt2结尾的
            has_smt2_data = False
            for value in df[col].values:
                if pd.notna(value) and str(value).strip().endswith('.smt2'):
                    has_smt2_data = True
                    break

            if has_smt2_data:
                smt2_cols.append(col)
            else:
                other_cols.append(col)

        print(f"包含.smt2数据的列: {smt2_cols}")

        # 重新排列列的顺序：.smt2列在前，其他列在后
        new_column_order = smt2_cols + other_cols
        df = df[new_column_order]

        print(f"最终数据形状: {df.shape}")
        print(f"最终列顺序: {list(df.columns)}")

        # 写回到原文件
        # 使用原文件的分隔符（这里假设是制表符，您可以根据实际情况调整）
        df.to_csv(file_path, sep=',', index=False, na_rep='NA', header=None)
        print(f"处理完成，结果已写入 {file_path}")

    except Exception as e:
        print(f"处理文件时出错: {e}")
        import traceback
        traceback.print_exc()


def main():
    file_path = '../resources/RQ2/QF_BV/z3/smt_feature.csv'
    process_file(file_path)


if __name__ == "__main__":
    main()