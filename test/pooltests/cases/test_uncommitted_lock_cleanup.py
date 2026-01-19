from pooltests.common import first_nonempty_line, parse_bool


def run(ctx):
    failures = []

    setup_sql = """
    SELECT pg_backend_pid();
    BEGIN;
    LOCK TABLE pg_class IN ACCESS EXCLUSIVE MODE;
    SELECT pg_try_advisory_lock(434343);
    SELECT 'LOCKED';
    """
    output_a = ctx.run_psql(setup_sql, title="Session A (Hold locks)")
    if output_a is None:
        return ["Session A 失败（加锁）"]

    lines_a = (output_a["stdout"] or "").split("\n")
    pid_a = first_nonempty_line(output_a["stdout"])
    if not pid_a or not pid_a.isdigit():
        return ["无法解析 Session A PID（加锁）"]

    verify_sql = f"""
    SELECT count(*) FROM pg_locks
      WHERE pid = {pid_a}
        AND locktype = 'relation'
        AND relation = 'pg_class'::regclass
        AND mode = 'AccessExclusiveLock';
    SELECT pg_try_advisory_lock(434343);
    SELECT pg_advisory_unlock(434343);
    """
    output_b = ctx.run_psql(verify_sql, title="Session B (Check leftover locks)")
    if output_b is None:
        return ["Session B 失败（检查锁）"]

    lines_b = (output_b["stdout"] or "").split("\n")
    if len(lines_b) < 2:
        return [f"Session B 输出行数不足，实际: {lines_b}"]

    relation_locks = lines_b[0].strip()
    adv_try = lines_b[1].strip()

    if relation_locks != "0":
        failures.append(f"未清理的 relation locks: {relation_locks}")

    if parse_bool(adv_try) is not True:
        failures.append("未清理的 advisory lock (434343)")

    return failures
