import os.path
import subprocess

from utils.config import CONFIG
from utils.logger import rq1_info_logger, rq1_error_logger

class SLOTDataRefiner:
    def __init__(self, smt_relative_path: str):
        self.smt_relative_path: str = smt_relative_path
        self.smt_path = os.path.join(CONFIG.dataset_dir, smt_relative_path)
        self.smt_opt_path = os.path.join(CONFIG.rq1_tmp_dir,
                                         f"{self.smt_relative_path.replace('/', '_').replace('.', '_')}-opt.smt2")

    def opt_with_slot(self):
        try:
            result = subprocess.run([
                CONFIG.original_slot_path,
                '-pall', '-m',
                '-s', self.smt_path,
                '-o', self.smt_opt_path,
            ], capture_output=True, text=True, timeout=CONFIG.TIMEOUT)
        except Exception:
            rq1_error_logger.error('[ORIGINAL SLOT TIMEOUT]',
                                   CONFIG.original_slot_path,
                                   '-s', self.smt_path,
                                   '-o', self.smt_opt_path)
        else:
            print(result.stdout)

def main():
    CONFIG.TIMEOUT = 600
    file_relative_paths = [
        "20170428-Liew-KLEE/imperial_svcomp_float-benchs_svcomp_sqrt_householder_interval.x86_64/query.6.smt2",
        "20170428-Liew-KLEE/imperial_synthetic_interval_klee_no_bug.x86_64/query.21.smt2",
        "20170428-Liew-KLEE/imperial_synthetic_interval_klee_no_bug.x86_64/query.24.smt2"
    ]

    for file_relative_path in file_relative_paths:
        slot_data_refiner = SLOTDataRefiner(file_relative_path)
        slot_data_refiner.opt_with_slot()

if __name__ == '__main__':
    main()