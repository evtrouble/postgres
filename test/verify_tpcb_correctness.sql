-- TPC-B Correctness Verification Script
-- This script verifies the core invariants of the TPC-B benchmark
--
-- Usage:
--   psql -f verify_tpcb_correctness.sql

\echo '========================================'
\echo 'TPC-B Correctness Verification'
\echo '========================================'
\echo ''

-- Check 1: Balance Conservation (Core Invariant)
\echo '1. Balance Conservation Check'
\echo '   (sum of all balances + sum of all deltas should be constant)'
\echo ''

SELECT 
    (SELECT sum(abalance) FROM pgbench_accounts) +
    (SELECT sum(tbalance) FROM pgbench_tellers) +
    (SELECT sum(bbalance) FROM pgbench_branches) AS total_balance,
    (SELECT sum(delta) FROM pgbench_history) AS total_delta,
    (SELECT sum(abalance) FROM pgbench_accounts) +
    (SELECT sum(tbalance) FROM pgbench_tellers) +
    (SELECT sum(bbalance) FROM pgbench_branches) +
    (SELECT sum(delta) FROM pgbench_history) AS grand_total;

\echo ''
\echo 'Expected: grand_total should remain constant across all transactions'
\echo ''

-- Check 2: No Negative Balances (Optional, depends on TPC-B variant)
\echo '2. Negative Balance Check'
\echo ''

SELECT 
    (SELECT count(*) FROM pgbench_accounts WHERE abalance < 0) AS negative_accounts,
    (SELECT count(*) FROM pgbench_tellers WHERE tbalance < 0) AS negative_tellers,
    (SELECT count(*) FROM pgbench_branches WHERE bbalance < 0) AS negative_branches;

\echo ''
\echo 'Note: Negative balances may be allowed in some TPC-B variants'
\echo ''

-- Check 3: Data Consistency
\echo '3. Data Consistency Checks'
\echo ''

-- Check if all accounts have valid branch/teller references
SELECT 
    (SELECT count(*) FROM pgbench_accounts a 
     WHERE NOT EXISTS (SELECT 1 FROM pgbench_branches b WHERE b.bid = a.bid)) 
    AS accounts_with_invalid_branch,
    (SELECT count(*) FROM pgbench_accounts a 
     WHERE NOT EXISTS (SELECT 1 FROM pgbench_tellers t WHERE t.tid = a.tid)) 
    AS accounts_with_invalid_teller;

\echo ''
\echo 'Expected: Both should be 0'
\echo ''

-- Check 4: Transaction Count
\echo '4. Transaction Statistics'
\echo ''

SELECT 
    (SELECT count(*) FROM pgbench_accounts) AS account_count,
    (SELECT count(*) FROM pgbench_tellers) AS teller_count,
    (SELECT count(*) FROM pgbench_branches) AS branch_count,
    (SELECT count(*) FROM pgbench_history) AS transaction_count;

\echo ''
\echo '========================================'
\echo 'Verification Complete'
\echo '========================================'

