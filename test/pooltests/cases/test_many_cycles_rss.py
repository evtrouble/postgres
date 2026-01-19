import time

from pooltests.common import first_nonempty_line, parse_bool


def _rss_summary(samples):
    if not samples:
        return "n/a"
    return f"min={min(samples)}KB max={max(samples)}KB last={samples[-1]}KB n={len(samples)}"


def run(ctx):
    failures = []

    setting = ctx.run_psql(
        "SELECT COALESCE(current_setting('enable_connection_pool', true), 'unknown');",
        title="检测 enable_connection_pool",
    )
    if setting is None:
        return ["无法执行 psql 检测 enable_connection_pool"]
    pool_setting = first_nonempty_line(setting["stdout"]) or "unknown"
    pool_enabled = pool_setting in ("on", "true", "1")
    print(f"enable_connection_pool: {pool_setting}")

    if not pool_enabled:
        print("SKIP: enable_connection_pool 未开启，跳过 RSS 多轮复用测试")
        return []

    pid_samples = {}
    pid_seen_order = []

    cycles = ctx.cycles
    warmup = ctx.rss_warmup
    max_delta_kb = ctx.rss_max_delta_kb

    print(f"cycles={cycles} warmup={warmup} rss_max_delta_kb={max_delta_kb}")

    for i in range(cycles):
        setup_sql = f"""
        SELECT pg_backend_pid();
        SET work_mem = '64MB';
        CREATE TEMP TABLE t{i}(id int);
        PREPARE p{i} AS SELECT {i};
        LISTEN c{i};
        SELECT pg_try_advisory_lock(900000 + {i});
        SELECT 'OK';
        """
        out_a = ctx.run_psql(setup_sql, title=f"cycle {i+1}/{cycles} - Session A", allow_error=False)
        if out_a is None:
            return failures + [f"cycle {i}: Session A 失败"]
        pid_a = first_nonempty_line(out_a["stdout"])
        if not pid_a or not pid_a.isdigit():
            return failures + [f"cycle {i}: 无法解析 Session A PID: {out_a['stdout']}"]

        verify_sql = f"""
        SELECT pg_backend_pid();
        SELECT count(*) FROM pg_prepared_statements WHERE name = 'p{i}';
        SELECT COALESCE(to_regclass('pg_temp.t{i}')::text, 'NONE');
        SELECT count(*) FROM pg_listening_channels() AS t(ch) WHERE ch = 'c{i}';
        SELECT pg_try_advisory_lock(900000 + {i});
        SELECT pg_advisory_unlock(900000 + {i});
        """
        out_b = ctx.run_psql(verify_sql, title=f"cycle {i+1}/{cycles} - Session B", allow_error=False)
        if out_b is None:
            return failures + [f"cycle {i}: Session B 失败"]

        lines = (out_b["stdout"] or "").split("\n")
        if len(lines) < 6:
            return failures + [f"cycle {i}: Session B 输出不足: {lines}"]

        pid_b = lines[0].strip()
        prep_count = lines[1].strip()
        temp_obj = lines[2].strip()
        listen_count = lines[3].strip()
        adv_try = lines[4].strip()

        if ctx.expect_reuse and pid_b != pid_a:
            failures.append(f"cycle {i}: 未复用（pid_a={pid_a}, pid_b={pid_b}）")

        if prep_count != "0":
            failures.append(f"cycle {i}: Prepared 未清理")
        if temp_obj and temp_obj not in ("", "NONE"):
            failures.append(f"cycle {i}: 临时表未清理")
        if listen_count != "0":
            failures.append(f"cycle {i}: LISTEN 未清理")
        if parse_bool(adv_try) is not True:
            failures.append(f"cycle {i}: advisory lock 未释放")

        if pid_b not in pid_samples:
            pid_samples[pid_b] = []
            pid_seen_order.append(pid_b)

        rss_kb = ctx.get_rss_kb(pid_b)
        if rss_kb is not None:
            pid_samples[pid_b].append(rss_kb)

        if (i + 1) % 10 == 0:
            print(f"progress: {i+1}/{cycles} pid={pid_b} rss={rss_kb}KB")

        time.sleep(0.01)

        if failures:
            break

    for pid in pid_seen_order:
        samples = pid_samples.get(pid) or []
        if len(samples) <= warmup + 2:
            print(f"pid={pid} rss: {_rss_summary(samples)} (样本不足，略过增长判断)")
            continue

        steady = samples[warmup:]
        min_kb = min(steady)
        last_kb = steady[-1]
        delta = last_kb - min_kb
        print(f"pid={pid} rss: {_rss_summary(samples)} steady_min={min_kb}KB steady_last={last_kb}KB delta={delta}KB")
        if delta > max_delta_kb:
            failures.append(f"pid={pid}: RSS 增长过大 delta={delta}KB > {max_delta_kb}KB")

    if not any(pid_samples.values()):
        print("SKIP: 无法读取 /proc/<pid>/status 的 VmRSS，跳过 RSS 判断")
        return [f for f in failures if "RSS" not in f]

    return failures

