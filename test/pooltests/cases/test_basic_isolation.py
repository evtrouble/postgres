from pooltests.common import first_nonempty_line, parse_bool


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

    setup_sql = """
    SELECT pg_backend_pid();
    SET work_mem = '64MB';
    SET statement_timeout = '5s';
    SET search_path = 'pg_temp';
    CREATE TEMP TABLE leakage_test(id int);
    PREPARE my_plan AS SELECT 100;
    DECLARE c1 CURSOR FOR SELECT 1;
    LISTEN leak_chan;
    SELECT pg_try_advisory_lock(424242);
    SELECT 'POLLUTION_DONE';
    """
    output_a = ctx.run_psql(setup_sql, title="Session A (Polluter)")
    if output_a is None:
        return ["Session A 失败"]

    pid_a = first_nonempty_line(output_a["stdout"])
    if not pid_a or not pid_a.isdigit():
        return ["无法解析 Session A 的 PID"]
    print(f"Session A PID: {pid_a}")

    verify_sql = """
    SELECT pg_backend_pid();
    SHOW work_mem;
    SHOW statement_timeout;
    SHOW search_path;
    SELECT count(*) FROM pg_prepared_statements WHERE name = 'my_plan';
    SELECT COALESCE(to_regclass('pg_temp.leakage_test')::text, 'NONE');
    SELECT count(*) FROM pg_listening_channels() AS t(ch) WHERE ch = 'leak_chan';
    SELECT count(*) FROM pg_cursors;
    SELECT pg_try_advisory_lock(424242);
    SELECT pg_advisory_unlock(424242);
    """
    output_b = ctx.run_psql(verify_sql, title="Session B (Verifier)")
    if output_b is None:
        return ["Session B 失败"]

    lines = (output_b["stdout"] or "").split("\n")
    if len(lines) < 10:
        return [f"Session B 输出行数不足，实际: {lines}"]

    pid_b = lines[0].strip()
    work_mem_b = lines[1].strip()
    statement_timeout_b = lines[2].strip()
    search_path_b = lines[3].strip()
    prep_stmt_count = lines[4].strip()
    temp_table_check = lines[5].strip()
    listen_count = lines[6].strip()
    cursors_count = lines[7].strip()
    advisory_try_lock = lines[8].strip()

    print(f"Session B PID: {pid_b}")

    if pid_a != pid_b and pool_enabled and ctx.expect_reuse:
        failures.append("连接未复用（PID 不同）")

    if work_mem_b == "64MB":
        failures.append("GUC work_mem 未重置")
    if statement_timeout_b == "5s":
        failures.append("GUC statement_timeout 未重置")
    if search_path_b == "pg_temp":
        failures.append("GUC search_path 未重置")

    if prep_stmt_count != "0":
        failures.append("Prepared Statement 未清理")

    if temp_table_check and temp_table_check not in ("", "NONE"):
        failures.append("临时表未清理")

    if listen_count != "0":
        failures.append("LISTEN 未清理")

    if cursors_count != "0":
        failures.append("Cursor/portal 未清理")

    if parse_bool(advisory_try_lock) is not True:
        failures.append("advisory lock 未释放")

    return failures

