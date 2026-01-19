import os
import subprocess
import sys


class PoolTestContext:
    def __init__(self):
        self.db_name = os.environ.get("PGDATABASE", "postgres")
        self.db_user = os.environ.get("PGUSER", os.environ.get("USER", "postgres"))
        self.db_host = os.environ.get("PGHOST", None)
        self.db_port = os.environ.get("PGPORT", None)
        self.expect_reuse = os.environ.get("EXPECT_REUSE", "1") not in ("0", "false", "False")
        self.cycles = int(os.environ.get("POOLTEST_CYCLES", "200"))
        self.rss_max_delta_kb = int(os.environ.get("POOLTEST_RSS_MAX_DELTA_KB", "20480"))
        self.rss_warmup = int(os.environ.get("POOLTEST_RSS_WARMUP", "10"))

    def get_psql_cmd(self):
        if "PSQL_PATH" in os.environ:
            return [os.environ["PSQL_PATH"]]

        from shutil import which

        if which("psql"):
            return ["psql"]

        script_dir = os.path.dirname(os.path.abspath(__file__))
        source_psql = os.path.join(script_dir, "../../src/bin/psql/psql")
        if os.path.exists(source_psql) and os.access(source_psql, os.X_OK):
            return [source_psql]

        print("错误: 未找到 psql 命令。请确保 psql 在 PATH 中，或者设置 PSQL_PATH 环境变量。")
        sys.exit(1)

    def run_psql(self, sql, title=None, allow_error=False):
        if title:
            print(f"--- {title} ---")

        cmd = self.get_psql_cmd() + [
            "-X",
            "-v",
            "ON_ERROR_STOP=1",
            "-d",
            self.db_name,
            "-U",
            self.db_user,
            "-t",
            "-A",
            "-c",
            sql,
        ]

        if self.db_host:
            cmd.extend(["-h", self.db_host])
        if self.db_port:
            cmd.extend(["-p", str(self.db_port)])

        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if (not allow_error) and result.returncode != 0:
            print(result.stderr)
            return None

        return {
            "returncode": result.returncode,
            "stdout": (result.stdout or "").strip(),
            "stderr": (result.stderr or "").strip(),
        }

    def get_rss_kb(self, pid):
        try:
            with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        parts = line.split()
                        if len(parts) >= 2 and parts[1].isdigit():
                            return int(parts[1])
                        return None
        except FileNotFoundError:
            return None
        except PermissionError:
            return None

        try:
            result = subprocess.run(
                ["ps", "-o", "rss=", "-p", str(pid)],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                return None
            s = (result.stdout or "").strip()
            if s.isdigit():
                return int(s)
        except Exception:
            return None

        return None


def first_nonempty_line(s):
    for line in (s or "").split("\n"):
        t = line.strip()
        if t:
            return t
    return None


def parse_bool(s):
    if s in ("t", "true", "True", "1", "on", "yes"):
        return True
    if s in ("f", "false", "False", "0", "off", "no"):
        return False
    return None
