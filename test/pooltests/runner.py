import importlib.util
import os
import sys

from .common import PoolTestContext


def _load_module_from_path(module_name, file_path):
    spec = importlib.util.spec_from_file_location(module_name, file_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _discover_case_files(cases_dir):
    files = []
    for name in sorted(os.listdir(cases_dir)):
        if name.startswith("test_") and name.endswith(".py"):
            files.append(os.path.join(cases_dir, name))
    return files


def run_all():
    ctx = PoolTestContext()
    cases_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases")
    case_files = _discover_case_files(cases_dir)
    if not case_files:
        print(f"未找到测试用例: {cases_dir}")
        return 1

    total_failures = 0
    for file_path in case_files:
        name = os.path.basename(file_path)
        module_name = f"pooltests.cases.{name[:-3]}"
        module = _load_module_from_path(module_name, file_path)
        if not hasattr(module, "run"):
            print(f"{name}: 缺少 run(ctx) 函数")
            total_failures += 1
            continue

        print(f"\n=== {name} ===")
        failures = module.run(ctx) or []
        if failures:
            total_failures += len(failures)
            print(f"{name}: 失败 {len(failures)} 项")
            for f in failures:
                print(f"- {f}")
        else:
            print(f"{name}: 通过")

    if total_failures:
        print(f"\n总失败数: {total_failures}")
        return 1

    print("\n全部测试通过")
    return 0


def main():
    return sys.exit(run_all())


if __name__ == "__main__":
    main()

