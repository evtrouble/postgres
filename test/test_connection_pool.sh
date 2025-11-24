#!/bin/bash
#-------------------------------------------------------------------------
# test_connection_pool.sh
#   Performance test script for connection pool
#
# This script tests connection pool performance by measuring:
# 1. Connection establishment time (with/without pool)
# 2. Process creation overhead
# 3. Concurrent connection handling
# 4. Resource usage
#
# Usage:
#   ./test_connection_pool.sh [test_name]
#
# Test names:
#   - connection_time: Measure connection establishment time
#   - concurrent: Test concurrent connections
#   - throughput: Test connection throughput
#   - all: Run all tests
#-------------------------------------------------------------------------

set -e

# Configuration
PGHOST=${PGHOST:-localhost}
PGPORT=${PGPORT:-5432}
PGUSER=${PGUSER:-$USER}
PGDATABASE=${PGDATABASE:-postgres}
TEST_COUNT=${TEST_COUNT:-100}
CONCURRENT_CONNS=${CONCURRENT_CONNS:-20}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test functions

# Test 1: Measure connection establishment time
test_connection_time() {
    echo -e "${GREEN}=== Test 1: Connection Establishment Time ===${NC}"
    echo ""
    
    echo "Testing connection time (this may take a while)..."
    echo ""
    
    # Create test script
    cat > /tmp/test_conn_time.sql <<EOF
\timing on
\set n 1
\set max :TEST_COUNT
SELECT 'Starting connection test...';
EOF
    
    # Measure connection time using psql
    echo "Measuring connection establishment time for $TEST_COUNT connections..."
    echo ""
    
    START_TIME=$(date +%s%N)
    
    for i in $(seq 1 $TEST_COUNT); do
        psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -c "SELECT 1;" > /dev/null 2>&1
    done
    
    END_TIME=$(date +%s%N)
    ELAPSED=$((($END_TIME - $START_TIME) / 1000000))  # Convert to milliseconds
    AVG_TIME=$(echo "scale=2; $ELAPSED / $TEST_COUNT" | bc)
    
    echo "Results:"
    echo "  Total connections: $TEST_COUNT"
    echo "  Total time: ${ELAPSED}ms"
    echo "  Average time per connection: ${AVG_TIME}ms"
    echo ""
    
    # Note: This is a baseline measurement
    # To see actual pool improvement, compare with pool enabled
    echo "  Note: This is baseline measurement (no pool)."
    echo "        To measure actual pool improvement:"
    echo "        1. Enable connection pool in postgresql.conf"
    echo "        2. Restart PostgreSQL"
    echo "        3. Run this test again"
    echo "        4. Compare the 'Average time per connection' values"
    echo ""
}

# Test 2: Concurrent connections
test_concurrent() {
    echo -e "${GREEN}=== Test 2: Concurrent Connection Handling ===${NC}"
    echo ""
    
    echo "Testing $CONCURRENT_CONNS concurrent connections..."
    echo ""
    
    START_TIME=$(date +%s%N)
    
    # Run concurrent connections
    for i in $(seq 1 $CONCURRENT_CONNS); do
        (
            psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
                 -c "SELECT pg_sleep(0.1), $i as conn_id;" > /dev/null 2>&1
        ) &
    done
    
    # Wait for all connections
    wait
    
    END_TIME=$(date +%s%N)
    ELAPSED=$((($END_TIME - $START_TIME) / 1000000))
    
    echo "Results:"
    echo "  Concurrent connections: $CONCURRENT_CONNS"
    echo "  Total time: ${ELAPSED}ms"
    echo "  Average time per connection: $(echo "scale=2; $ELAPSED / $CONCURRENT_CONNS" | bc)ms"
    echo ""
}

# Test 3: Connection throughput
test_throughput() {
    echo -e "${GREEN}=== Test 3: Connection Throughput ===${NC}"
    echo ""
    
    echo "Testing connection throughput (connections per second)..."
    echo ""
    
    START_TIME=$(date +%s%N)
    SUCCESS_COUNT=0
    
    for i in $(seq 1 $TEST_COUNT); do
        if psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
               -c "SELECT 1;" > /dev/null 2>&1; then
            SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
        fi
    done
    
    END_TIME=$(date +%s%N)
    ELAPSED=$((($END_TIME - $START_TIME) / 1000000))
    THROUGHPUT=$(echo "scale=2; $SUCCESS_COUNT * 1000 / $ELAPSED" | bc)
    
    echo "Results:"
    echo "  Successful connections: $SUCCESS_COUNT / $TEST_COUNT"
    echo "  Total time: ${ELAPSED}ms"
    echo "  Throughput: ${THROUGHPUT} connections/second"
    echo ""
}

# Test 4: Process creation overhead (indirect)
test_process_overhead() {
    echo -e "${GREEN}=== Test 4: Process Creation Overhead (Indirect) ===${NC}"
    echo ""
    
    echo "Measuring backend process count during connection burst..."
    echo ""
    
    # Get initial process count
    INITIAL_COUNT=$(ps aux | grep -c "[p]ostgres:.*$PGDATABASE" || echo "0")
    echo "Initial backend processes: $INITIAL_COUNT"
    
    # Create burst of connections
    echo "Creating burst of $TEST_COUNT connections..."
    for i in $(seq 1 $TEST_COUNT); do
        psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
             -c "SELECT pg_sleep(0.01);" > /dev/null 2>&1 &
    done
    
    sleep 1
    
    # Get peak process count
    PEAK_COUNT=$(ps aux | grep -c "[p]ostgres:.*$PGDATABASE" || echo "0")
    echo "Peak backend processes: $PEAK_COUNT"
    
    sleep 2
    
    # Get final process count
    FINAL_COUNT=$(ps aux | grep -c "[p]ostgres:.*$PGDATABASE" || echo "0")
    echo "Final backend processes: $FINAL_COUNT"
    echo ""
    
    echo "Analysis:"
    echo "  Process creation overhead: $((PEAK_COUNT - INITIAL_COUNT)) processes"
    echo "  With connection pool, this should be limited to pool size"
    echo ""
}

# Test 5: Connection reuse (if pool is working)
test_connection_reuse() {
    echo -e "${GREEN}=== Test 5: Connection Reuse Detection ===${NC}"
    echo ""
    
    echo "Testing if connections are being reused..."
    echo ""
    
    # Get backend PIDs before
    psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -t -c \
         "SELECT pid FROM pg_stat_activity WHERE datname = '$PGDATABASE' AND pid != pg_backend_pid();" \
         > /tmp/pids_before.txt 2>/dev/null || true
    
    BEFORE_COUNT=$(wc -l < /tmp/pids_before.txt | tr -d ' ')
    echo "Backend processes before: $BEFORE_COUNT"
    
    # Make multiple connections
    for i in $(seq 1 10); do
        psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
             -c "SELECT pg_sleep(0.1);" > /dev/null 2>&1
    done
    
    sleep 0.5
    
    # Get backend PIDs after
    psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -t -c \
         "SELECT pid FROM pg_stat_activity WHERE datname = '$PGDATABASE' AND pid != pg_backend_pid();" \
         > /tmp/pids_after.txt 2>/dev/null || true
    
    AFTER_COUNT=$(wc -l < /tmp/pids_after.txt | tr -d ' ')
    echo "Backend processes after: $AFTER_COUNT"
    echo ""
    
    if [ "$AFTER_COUNT" -le "$BEFORE_COUNT" ]; then
        echo -e "${GREEN}✓ Possible connection reuse detected${NC}"
        echo "  Process count did not increase significantly"
    else
        echo -e "${YELLOW}⚠ New processes created${NC}"
        echo "  Process count increased by $((AFTER_COUNT - BEFORE_COUNT))"
    fi
    echo ""
}

# Main test runner
main() {
    TEST_NAME=${1:-all}
    
    echo "=========================================="
    echo "Connection Pool Performance Test"
    echo "=========================================="
    echo ""
    echo "Configuration:"
    echo "  Host: $PGHOST"
    echo "  Port: $PGPORT"
    echo "  Database: $PGDATABASE"
    echo "  Test count: $TEST_COUNT"
    echo "  Concurrent connections: $CONCURRENT_CONNS"
    echo ""
    echo "Note: These tests measure indirect indicators of connection pool"
    echo "      performance. Actual pool behavior depends on implementation."
    echo ""
    echo "=========================================="
    echo ""
    
    case $TEST_NAME in
        connection_time)
            test_connection_time
            ;;
        concurrent)
            test_concurrent
            ;;
        throughput)
            test_throughput
            ;;
        process_overhead)
            test_process_overhead
            ;;
        connection_reuse)
            test_connection_reuse
            ;;
        all)
            test_connection_time
            test_concurrent
            test_throughput
            test_process_overhead
            test_connection_reuse
            ;;
        *)
            echo "Unknown test: $TEST_NAME"
            echo "Available tests: connection_time, concurrent, throughput, process_overhead, connection_reuse, all"
            exit 1
            ;;
    esac
    
    echo -e "${GREEN}=== Test Complete ===${NC}"
    echo ""
    echo "To measure connection pool performance:"
    echo "  1. Run this test now (baseline, no pool) and save results"
    echo "  2. Enable connection pool:"
    echo "     ALTER SYSTEM SET enable_connection_pool = true;"
    echo "     ALTER SYSTEM SET connection_pool_size = 10;"
    echo "     (restart PostgreSQL)"
    echo "  3. Run this test again and compare results"
    echo ""
    echo "Key metrics to compare:"
    echo "  - Average time per connection (should decrease with pool)"
    echo "  - Total time (should decrease with pool)"
    echo "  - Process count (should be limited by pool size)"
    echo ""
}

# Run main function
main "$@"

