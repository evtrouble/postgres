from pooltests.common import first_nonempty_line


def run(ctx):
    failures = []

    setup_sql = """
    SELECT pg_backend_pid();
    SET work_mem = '64MB';
    CREATE TEMP TABLE err_leak(id int);
    PREPARE err_plan AS SELECT 1;
    LISTEN err_chan;
    BEGIN;
    SELECT 1/0;
    COMMIT;
    """
    output_a = ctx.run_psql(setup_sql, title="Session A (Expected ERROR)", allow_error=True)
    if output_a is None:
        return ["Session A 无输出"]

    if output_a["returncode"] == 0:
        failures.append("预期 Session A 发生 ERROR，但返回码为 0")

    pid_a = first_nonempty_line(output_a["stdout"])
    if not pid_a or not pid_a.isdigit():
        failures.append("无法解析 Session A PID（ERROR 场景）")
        return failures

    verify_sql = """
    SELECT pg_backend_pid();
    SHOW work_mem;
    SELECT count(*) FROM pg_prepared_statements WHERE name = 'err_plan';
    SELECT COALESCE(to_regclass('pg_temp.err_leak')::text, 'NONE');
    SELECT count(*) FROM pg_listening_channels() AS t(ch) WHERE ch = 'err_chan';
    SELECT txid_current_if_assigned() IS NULL;
    """
    output_b = ctx.run_psql(verify_sql, title="Session B (Verify after ERROR)")
    if output_b is None:
        return failures + ["Session B 失败（ERROR 后验证）"]

    lines = (output_b["stdout"] or "").split("\n")
    if len(lines) < 6:
        return failures + [f"Session B 输出行数不足，实际: {lines}"]

    pid_b = lines[0].strip()
    work_mem_b = lines[1].strip()
    prep_stmt_count = lines[2].strip()
    temp_table_check = lines[3].strip()
    listen_count = lines[4].strip()
    txid_assigned_is_null = lines[5].strip()

    if pid_a != pid_b and ctx.expect_reuse:
        failures.append("ERROR 场景后连接未复用（PID 不同）")

    if work_mem_b == "64MB":
        failures.append("ERROR 场景后 work_mem 未重置")
    if prep_stmt_count != "0":
        failures.append("ERROR 场景后 Prepared Statement 未清理")
    if temp_table_check and temp_table_check not in ("", "NONE"):
        failures.append("ERROR 场景后临时表未清理")
    if listen_count != "0":
        failures.append("ERROR 场景后 LISTEN 未清理")
    if txid_assigned_is_null != "t":
        failures.append("ERROR 场景后仍残留事务状态")

    return failures

