import sys
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)

from pooltests.runner import run_all


if __name__ == "__main__":
    sys.exit(run_all())
