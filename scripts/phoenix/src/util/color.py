# Colors
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
BLUE = "\033[34m"
MAGENTA = "\033[35m"
CYAN = "\033[36m"
WHITE = "\033[37m"
RESET = "\033[39m"

# Colors - extended
PURPLE = "\033[38;5;93m"
PINK = "\033[38;5;205m"
ORANGE = "\033[38;5;214m"
GOLD = "\033[38;5;220m"
GRAY = "\033[38;5;245m"


def colored_write_line(text: str, color_code: str = RESET, flush: bool = True):
    print(color_code + text + RESET, flush=flush)
