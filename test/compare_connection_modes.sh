#!/bin/bash
#-------------------------------------------------------------------------
# compare_connection_modes.sh
#   Compare performance of 4 connection modes:
#   1. No pool, long connections (pgbench default)
#   2. No pool, short connections (pgbench -C)
#   3. pgBouncer connection pool (via proxy)
#   4. Built-in connection pool (your implementation)
#
# Usage:
#   ./compare_connection_modes.sh [options]
#
# Options:
#   --clients N      Number of concurrent clients (default: 20)
#   --duration N     Test duration in seconds (default: 60)
#   --scale N        Scale factor (default: 1)
#   --pgbouncer-port PORT  pgBouncer port (default: 6432)
#   --skip-mode MODE Skip a mode (1,2,3,4 or all)
#-------------------------------------------------------------------------

set -e

# Default values
CLIENTS=20
DURATION=60
SCALE=1
PGBOUNCER_PORT=6432
PGHOST=${PGHOST:-localhost}
PGPORT=${PGPORT:-5432}
PGBOUNCER_HOST=${PGBOUNCER_HOST:-localhost}
PGUSER=${PGUSER:-$USER}
PGDATABASE=${PGDATABASE:-postgres}
RESULTS_DIR="./results_$(date +%Y%m%d_%H%M%S)"
SKIP_MODES=""

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clients)
            CLIENTS="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --scale)
            SCALE="$2"
            shift 2
            ;;
        --pgbouncer-port)
            PGBOUNCER_PORT="$2"
            shift 2
            ;;
        --skip-mode)
            SKIP_MODES="$SKIP_MODES $2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Create results directory
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Connection Mode Performance Comparison"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Clients: $CLIENTS"
echo "  Duration: ${DURATION}s"
echo "  Scale: $SCALE"
echo "  Results directory: $RESULTS_DIR"
echo ""
echo "Testing 4 modes:"
echo "  1. No pool, long connections (pgbench default)"
echo "  2. No pool, short connections (pgbench -C)"
echo "  3. pgBouncer connection pool"
echo "  4. Built-in connection pool"
echo ""

# Check if pgbench is available
if ! command -v pgbench >/dev/null 2>&1; then
    echo -e "${RED}Error: pgbench not found in PATH${NC}"
    exit 1
fi

# Check PostgreSQL connection
if ! psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -c "SELECT 1;" >/dev/null 2>&1; then
    echo -e "${RED}Error: Cannot connect to PostgreSQL${NC}"
    echo "  Host: $PGHOST"
    echo "  Port: $PGPORT"
    echo "  User: $PGUSER"
    echo "  Database: $PGDATABASE"
    exit 1
fi

# Initialize pgbench database
echo -e "${BLUE}Initializing pgbench database...${NC}"
pgbench -h $PGHOST -p $PGPORT -U $PGUSER -i -s $SCALE $PGDATABASE >/dev/null 2>&1
echo "Done."
echo ""

# Function to check if mode should be skipped
should_skip() {
    local mode=$1
    for skip in $SKIP_MODES; do
        if [ "$skip" = "$mode" ]; then
            return 0
        fi
    done
    return 1
}

# Function to verify data correctness (TPC-B invariants)
verify_correctness() {
    local mode=$1
    local test_host=${2:-$PGHOST}
    local test_port=${3:-$PGPORT}
    local correctness_file="$RESULTS_DIR/mode_${mode}_correctness.txt"
    
    echo -e "${BLUE}Verifying data correctness...${NC}"
    
    # Get initial balance before test
    local initial_balance=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT (SELECT sum(abalance) FROM pgbench_accounts) + 
                (SELECT sum(tbalance) FROM pgbench_tellers) + 
                (SELECT sum(bbalance) FROM pgbench_branches);" 2>/dev/null | tr -d ' ' || echo "ERROR")
    
    # Get total delta from history
    local total_delta=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT sum(delta) FROM pgbench_history;" 2>/dev/null | tr -d ' ' || echo "ERROR")
    
    # Get final balance after test
    local final_balance=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT (SELECT sum(abalance) FROM pgbench_accounts) + 
                (SELECT sum(tbalance) FROM pgbench_tellers) + 
                (SELECT sum(bbalance) FROM pgbench_branches);" 2>/dev/null | tr -d ' ' || echo "ERROR")
    
    # Calculate expected balance
    local expected_balance=$(echo "$initial_balance + $total_delta" | bc 2>/dev/null || echo "ERROR")
    
    # Check if balances match (TPC-B invariant: total_balance + total_delta should be constant)
    cat > "$correctness_file" <<EOF
TPC-B Correctness Verification for Mode $mode
=============================================

Initial Balance (sum of accounts + tellers + branches): $initial_balance
Total Delta (sum of history.delta): $total_delta
Expected Final Balance: $expected_balance
Actual Final Balance: $final_balance

EOF
    
    # Verify balance conservation
    if [ "$final_balance" != "ERROR" ] && [ "$expected_balance" != "ERROR" ]; then
        local balance_diff=$(echo "scale=2; $final_balance - $expected_balance" | bc 2>/dev/null || echo "ERROR")
        
        if [ "$balance_diff" = "0" ] || [ "$balance_diff" = "0.00" ] || [ "$balance_diff" = "-0.00" ]; then
            echo -e "${GREEN}✓ Balance conservation: PASSED${NC}" | tee -a "$correctness_file"
            echo "  Difference: $balance_diff" | tee -a "$correctness_file"
        else
            echo -e "${RED}✗ Balance conservation: FAILED${NC}" | tee -a "$correctness_file"
            echo "  Difference: $balance_diff" | tee -a "$correctness_file"
            echo "  This indicates data corruption or transaction errors!" | tee -a "$correctness_file"
        fi
    else
        echo -e "${RED}✗ Balance conservation: ERROR (could not calculate)${NC}" | tee -a "$correctness_file"
    fi
    
    # Additional checks
    echo "" | tee -a "$correctness_file"
    echo "Additional Checks:" | tee -a "$correctness_file"
    
    # Check for negative balances (should not happen in TPC-B)
    local negative_accounts=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT count(*) FROM pgbench_accounts WHERE abalance < 0;" 2>/dev/null | tr -d ' ' || echo "ERROR")
    local negative_tellers=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT count(*) FROM pgbench_tellers WHERE tbalance < 0;" 2>/dev/null | tr -d ' ' || echo "ERROR")
    local negative_branches=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT count(*) FROM pgbench_branches WHERE bbalance < 0;" 2>/dev/null | tr -d ' ' || echo "ERROR")
    
    echo "  Negative account balances: $negative_accounts" | tee -a "$correctness_file"
    echo "  Negative teller balances: $negative_tellers" | tee -a "$correctness_file"
    echo "  Negative branch balances: $negative_branches" | tee -a "$correctness_file"
    
    if [ "$negative_accounts" = "0" ] && [ "$negative_tellers" = "0" ] && [ "$negative_branches" = "0" ]; then
        echo -e "${GREEN}✓ No negative balances: PASSED${NC}" | tee -a "$correctness_file"
    else
        echo -e "${YELLOW}⚠ Negative balances detected (may be normal in TPC-B)${NC}" | tee -a "$correctness_file"
    fi
    
    # Check transaction count consistency
    local history_count=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT count(*) FROM pgbench_history;" 2>/dev/null | tr -d ' ' || echo "ERROR")
    
    echo "" | tee -a "$correctness_file"
    echo "Transaction Statistics:" | tee -a "$correctness_file"
    echo "  History records: $history_count" | tee -a "$correctness_file"
    
    echo ""
}

# Function to run pgbench and collect results
run_pgbench() {
    local mode=$1
    local description=$2
    local pgbench_args=$3
    local test_host=${4:-$PGHOST}
    local test_port=${5:-$PGPORT}
    local output_file="$RESULTS_DIR/mode_${mode}.txt"
    
    echo -e "${GREEN}=== Mode $mode: $description ===${NC}"
    echo "Running pgbench..."
    echo "  Host: $test_host"
    echo "  Port: $test_port"
    echo "  Args: $pgbench_args"
    echo ""
    
    # Get initial state for correctness check
    local initial_balance=$(psql -h "$test_host" -p "$test_port" -U $PGUSER -d $PGDATABASE -t -c \
        "SELECT (SELECT sum(abalance) FROM pgbench_accounts) + 
                (SELECT sum(tbalance) FROM pgbench_tellers) + 
                (SELECT sum(bbalance) FROM pgbench_branches);" 2>/dev/null | tr -d ' ' || echo "0")
    
    # Run pgbench
    pgbench -h "$test_host" -p "$test_port" -U $PGUSER \
            -c $CLIENTS \
            -j $CLIENTS \
            -T $DURATION \
            -r \
            $pgbench_args \
            $PGDATABASE > "$output_file" 2>&1
    
    # Extract key metrics
    local tps=$(grep "tps = " "$output_file" | grep -v "including" | awk '{print $3}')
    local latency=$(grep "latency average = " "$output_file" | awk '{print $4}')
    local latency_stddev=$(grep "latency stddev = " "$output_file" | awk '{print $4}')
    
    echo "Performance Results:"
    echo "  TPS: $tps"
    echo "  Latency (avg): $latency"
    echo "  Latency (stddev): $latency_stddev"
    echo ""
    
    # Verify correctness
    verify_correctness "$mode" "$test_host" "$test_port"
    
    # Save summary
    echo "Mode $mode: $description" >> "$RESULTS_DIR/summary.txt"
    echo "  TPS: $tps" >> "$RESULTS_DIR/summary.txt"
    echo "  Latency (avg): $latency" >> "$RESULTS_DIR/summary.txt"
    echo "  Latency (stddev): $latency_stddev" >> "$RESULTS_DIR/summary.txt"
    
    # Add correctness summary
    local balance_ok=$(grep "Balance conservation: PASSED" "$RESULTS_DIR/mode_${mode}_correctness.txt" >/dev/null && echo "PASSED" || echo "FAILED")
    echo "  Correctness: $balance_ok" >> "$RESULTS_DIR/summary.txt"
    echo "" >> "$RESULTS_DIR/summary.txt"
}

# Mode 1: No pool, long connections (pgbench default)
if ! should_skip 1; then
    echo -e "${YELLOW}Preparing Mode 1: Disable built-in pool if enabled...${NC}"
    psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -c \
         "ALTER SYSTEM SET enable_connection_pool = false;" >/dev/null 2>&1 || true
    # Note: This requires restart, but we'll note it
    echo "  Note: ensure enable_connection_pool = false in postgresql.conf"
    echo ""
    
    run_pgbench 1 "No pool, long connections" ""
    
    sleep 2
fi

# Mode 2: No pool, short connections (pgbench -C)
if ! should_skip 2; then
    echo -e "${YELLOW}Preparing Mode 2: Ensure pool is disabled...${NC}"
    echo "  (Same as Mode 1, but using -C flag)"
    echo ""
    
    run_pgbench 2 "No pool, short connections" "-C"
    
    sleep 2
fi

# Mode 3: pgBouncer connection pool
if ! should_skip 3; then
    echo -e "${YELLOW}Preparing Mode 3: Testing via pgBouncer...${NC}"
    
    # Check if pgBouncer is running
    if ! psql -h $PGBOUNCER_HOST -p $PGBOUNCER_PORT -U $PGUSER -d $PGDATABASE \
         -c "SELECT 1;" >/dev/null 2>&1; then
        echo -e "${RED}Warning: Cannot connect to pgBouncer at $PGBOUNCER_HOST:$PGBOUNCER_PORT${NC}"
        echo "  Skipping Mode 3. Please start pgBouncer or use --skip-mode 3"
        echo ""
    else
        echo "  pgBouncer connection OK"
        echo ""
        
        run_pgbench 3 "pgBouncer connection pool" "-C" "$PGBOUNCER_HOST" "$PGBOUNCER_PORT"
        
        sleep 2
    fi
fi

# Mode 4: Built-in connection pool
if ! should_skip 4; then
    echo -e "${YELLOW}Preparing Mode 4: Enable built-in connection pool...${NC}"
    echo "  Setting enable_connection_pool = true"
    echo "  Setting connection_pool_size = $CLIENTS"
    echo ""
    echo -e "${RED}IMPORTANT: You need to:${NC}"
    echo "  1. Set enable_connection_pool = true in postgresql.conf"
    echo "  2. Set connection_pool_size = $CLIENTS (or appropriate value)"
    echo "  3. Restart PostgreSQL"
    echo "  4. Press Enter to continue..."
    read -r
    
    # Verify pool is enabled
    local pool_enabled=$(psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -t -c \
                          "SHOW enable_connection_pool;" 2>/dev/null | tr -d ' ' || echo "off")
    
    if [ "$pool_enabled" != "on" ]; then
        echo -e "${RED}Warning: enable_connection_pool is not 'on'${NC}"
        echo "  Current value: $pool_enabled"
        echo "  Continuing anyway..."
        echo ""
    fi
    
    run_pgbench 4 "Built-in connection pool" "-C"
    
    sleep 2
fi

# Generate comparison report
echo ""
echo "=========================================="
echo -e "${GREEN}Generating Comparison Report${NC}"
echo "=========================================="
echo ""

cat > "$RESULTS_DIR/comparison.txt" <<EOF
Connection Mode Performance Comparison
==========================================

Test Configuration:
  Clients: $CLIENTS
  Duration: ${DURATION}s
  Scale: $SCALE
  Test Date: $(date)

Results Summary:
EOF

cat "$RESULTS_DIR/summary.txt" >> "$RESULTS_DIR/comparison.txt"

cat >> "$RESULTS_DIR/comparison.txt" <<EOF

Detailed Results:
  Mode 1: $RESULTS_DIR/mode_1.txt
  Mode 2: $RESULTS_DIR/mode_2.txt
  Mode 3: $RESULTS_DIR/mode_3.txt
  Mode 4: $RESULTS_DIR/mode_4.txt

Correctness Verification:
  Mode 1: $RESULTS_DIR/mode_1_correctness.txt
  Mode 2: $RESULTS_DIR/mode_2_correctness.txt
  Mode 3: $RESULTS_DIR/mode_3_correctness.txt
  Mode 4: $RESULTS_DIR/mode_4_correctness.txt

Key Metrics to Compare:
  1. TPS (Transactions Per Second) - Higher is better
  2. Latency (average) - Lower is better
  3. Latency (stddev) - Lower is better (more consistent)
  4. Correctness - Balance conservation must PASS for all modes

Correctness Check (TPC-B Invariant):
  The sum of (accounts.balance + tellers.balance + branches.balance) 
  plus the sum of history.delta should remain constant.
  This verifies that transactions are correctly applied.

Expected Results:
  Mode 1 (long conn): Best TPS, lowest latency, PASS correctness
  Mode 2 (short conn): Worst TPS, highest latency, PASS correctness
  Mode 3 (pgBouncer): Good TPS, low latency, PASS correctness
  Mode 4 (built-in): Should be close to Mode 1, better than Mode 2, PASS correctness

⚠️  IMPORTANT: If any mode shows FAILED correctness, there is a serious bug!
EOF

cat "$RESULTS_DIR/comparison.txt"

echo ""
echo -e "${GREEN}Results saved to: $RESULTS_DIR${NC}"
echo ""
echo "To view detailed results:"
echo "  cat $RESULTS_DIR/comparison.txt"
echo "  cat $RESULTS_DIR/mode_*.txt"

