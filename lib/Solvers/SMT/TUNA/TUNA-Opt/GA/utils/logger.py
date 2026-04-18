"""
Author: Jiachen Lu<lujc@zju.edu.cn>
File Description: 
Creation Date: 2024/11/7
"""
import logging
import os
import sys
from logging.handlers import TimedRotatingFileHandler

from asgi_correlation_id import correlation_id

HOME_PATH = os.environ.get('HOME')
LOG_PATH = os.path.join(HOME_PATH, 'logs')
RQ1_LOG_PATH = os.path.join(LOG_PATH, 'RQ1')


class TracerFilter(logging.Filter):
    def filter(self, record):
        # 将 correlation_id 添加到日志记录(record)中
        record.correlation_id = correlation_id.get()
        return True


def setup_logger(name, log_file, level):
    logger = logging.getLogger(name)
    logger.setLevel(level)

    # 只有当handler未被添加时，才添加handler
    if not logger.handlers:
        # 创建文件处理器并设置等级
        file_handler = TimedRotatingFileHandler(log_file, when='midnight', interval=1, backupCount=7, encoding='utf-8')
        file_handler.setLevel(level)

        # 创建日志格式化器
        formatter = logging.Formatter(f'%(asctime)s - %(levelname)s [TraceID: %(correlation_id)s] - %(message)s')
        file_handler.setFormatter(formatter)

        # 添加文件处理器到logger
        logger.addHandler(file_handler)

        # 创建并添加一个 StreamHandler 实例以便同时在控制台输出日志
        stdout_handler = logging.StreamHandler(stream=sys.stdout)
        stdout_handler.setFormatter(formatter)
        logger.addHandler(stdout_handler)

        # 防止向上（root logger）冒泡
        logger.propagate = False

    return logger


os.makedirs(RQ1_LOG_PATH, exist_ok=True)


# Logger
rq1_info_logger = setup_logger('rq1_info', os.path.join(RQ1_LOG_PATH, 'rq1.info'), logging.INFO)
rq1_error_logger = setup_logger('rq1_error', os.path.join(RQ1_LOG_PATH, 'rq1.error'), logging.ERROR)

# Tracer
tracer_filter = TracerFilter()
rq1_info_logger.addFilter(tracer_filter)
rq1_error_logger.addFilter(tracer_filter)