import json
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
        self.mcxt_max_delta_bytes = int(os.environ.get("POOLTEST_MCXT_MAX_DELTA_BYTES", str(100 * 1024 * 1024)))
        self.mcxt_warmup = int(os.environ.get("POOLTEST_MCXT_WARMUP", str(self.rss_warmup)))
        self.mcxt_snapshot_every = int(os.environ.get("POOLTEST_MCXT_SNAPSHOT_EVERY", "0"))
        self.mcxt_topn = int(os.environ.get("POOLTEST_MCXT_TOPN", "10"))
        self.rss_smaps = os.environ.get("POOLTEST_RSS_SMAPS", "0") not in ("0", "false", "False")
        self.fd_count = os.environ.get("POOLTEST_FD_COUNT", "0") not in ("0", "false", "False")

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

    def can_read_backend_memory_contexts(self):
        res = self.run_psql("SELECT 1 FROM pg_backend_memory_contexts LIMIT 1;", allow_error=True)
        return res is not None and res["returncode"] == 0

    def get_backend_memory_context_totals(self):
        res = self.run_psql(
            "SELECT sum(total_bytes)::bigint, sum(used_bytes)::bigint FROM pg_backend_memory_contexts;",
            allow_error=True,
        )
        if res is None or res["returncode"] != 0:
            return None

        line = first_nonempty_line(res["stdout"] or "")
        if not line:
            return None

        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 2:
            return None

        total_s, used_s = parts
        if not total_s.lstrip("-").isdigit() or not used_s.lstrip("-").isdigit():
            return None

        return int(total_s), int(used_s)

    def get_backend_memory_context_top(self, topn=None):
        if topn is None:
            topn = self.mcxt_topn
        if topn <= 0:
            return None

        res = self.run_psql(
            f"""
            SELECT pg_backend_pid();
            SELECT COALESCE(
                jsonb_agg(
                    jsonb_build_object('name', name, 'total', total_bytes, 'used', used_bytes)
                    ORDER BY total_bytes DESC
                )::text,
                '[]'
            )
            FROM (
                SELECT name,
                       sum(total_bytes)::bigint AS total_bytes,
                       sum(used_bytes)::bigint AS used_bytes
                FROM pg_backend_memory_contexts
                GROUP BY name
                ORDER BY sum(total_bytes) DESC
                LIMIT {int(topn)}
            ) s;
            """,
            allow_error=True,
        )
        if res is None or res["returncode"] != 0:
            return None

        lines = (res["stdout"] or "").split("\n")
        pid = first_nonempty_line(lines[0] if lines else "")
        payload = first_nonempty_line(lines[1] if len(lines) > 1 else "")
        if not pid or not pid.isdigit() or payload is None:
            return None

        try:
            items = json.loads(payload)
        except Exception:
            return None

        if not isinstance(items, list):
            return None

        normalized = []
        for it in items:
            if not isinstance(it, dict):
                continue
            name = it.get("name")
            total = it.get("total")
            used = it.get("used")
            if isinstance(name, str) and isinstance(total, int) and isinstance(used, int):
                normalized.append({"name": name, "total": total, "used": used})
        return pid, normalized

    def get_smaps_rollup_kb(self, pid):
        wanted = {
            "Rss": "Rss",
            "Pss": "Pss",
            "Shared_Clean": "Shared_Clean",
            "Shared_Dirty": "Shared_Dirty",
            "Private_Clean": "Private_Clean",
            "Private_Dirty": "Private_Dirty",
            "Swap": "Swap",
        }
        out = {}
        try:
            with open(f"/proc/{pid}/smaps_rollup", "r", encoding="utf-8") as f:
                for line in f:
                    if ":" not in line:
                        continue
                    k, rest = line.split(":", 1)
                    k = k.strip()
                    if k not in wanted:
                        continue
                    parts = rest.split()
                    if len(parts) >= 1 and parts[0].isdigit():
                        out[k] = int(parts[0])
        except FileNotFoundError:
            return None
        except PermissionError:
            return None
        except Exception:
            return None

        return out or None

    def get_fd_count(self, pid):
        try:
            return len(os.listdir(f"/proc/{pid}/fd"))
        except FileNotFoundError:
            return None
        except PermissionError:
            return None
        except Exception:
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
