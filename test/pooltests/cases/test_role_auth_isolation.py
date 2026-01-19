from pooltests.common import first_nonempty_line


def run(ctx):
    failures = []
    role_name = "pooltest_role"

    super_sql = "SELECT rolsuper FROM pg_roles WHERE rolname = current_user;"
    super_res = ctx.run_psql(super_sql, title="检测当前用户是否为 superuser")
    if super_res is None:
        return ["无法检测当前用户权限"]
    is_super = first_nonempty_line(super_res["stdout"]) == "t"
    print(f"superuser: {is_super}")

    if is_super:
        create_sql = f"""
        DO $$
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '{role_name}') THEN
                CREATE ROLE {role_name} LOGIN;
            END IF;
        END
        $$;
        """
        if ctx.run_psql(create_sql, title=f"准备测试角色 {role_name}") is None:
            return [f"无法创建测试角色 {role_name}"]
    else:
        exists_res = ctx.run_psql(
            f"SELECT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '{role_name}');",
            title=f"检查测试角色 {role_name} 是否存在",
        )
        if exists_res is None or first_nonempty_line(exists_res["stdout"]) != "t":
            print(f"SKIP: 不是 superuser 且角色 {role_name} 不存在，跳过 role/auth 隔离测试")
            return []

    a_sql = f"""
    SELECT pg_backend_pid();
    SELECT current_user, session_user;
    SET ROLE {role_name};
    SELECT current_user, session_user;
    SET SESSION AUTHORIZATION {role_name};
    SELECT current_user, session_user;
    SELECT 'DONE';
    """
    out_a = ctx.run_psql(a_sql, title="Session A (Change role/auth)")
    if out_a is None:
        return ["Session A 失败（SET ROLE / SET SESSION AUTHORIZATION）"]

    lines_a = (out_a["stdout"] or "").split("\n")
    if len(lines_a) < 4:
        return [f"Session A 输出行数不足，实际: {lines_a}"]

    pid_a = lines_a[0].strip()
    if not pid_a.isdigit():
        failures.append("无法解析 Session A PID")

    b_sql = """
    SELECT pg_backend_pid();
    SELECT current_user, session_user;
    """
    out_b = ctx.run_psql(b_sql, title="Session B (Verify role/auth reset)")
    if out_b is None:
        return failures + ["Session B 失败（验证 role/auth）"]

    lines_b = (out_b["stdout"] or "").split("\n")
    if len(lines_b) < 2:
        return failures + [f"Session B 输出行数不足，实际: {lines_b}"]

    pid_b = lines_b[0].strip()
    users = lines_b[1].strip()
    parts = users.split("|")
    if len(parts) != 2:
        failures.append(f"无法解析 Session B 用户信息: {users}")
        return failures

    current_user_b = parts[0]
    session_user_b = parts[1]

    if ctx.expect_reuse and pid_a.isdigit() and pid_b.isdigit() and pid_a != pid_b:
        failures.append("role/auth 测试中连接未复用（PID 不同）")

    if current_user_b != ctx.db_user:
        failures.append(f"current_user 未重置，期望 {ctx.db_user}，实际 {current_user_b}")
    if session_user_b != ctx.db_user:
        failures.append(f"session_user 未重置，期望 {ctx.db_user}，实际 {session_user_b}")

    return failures

