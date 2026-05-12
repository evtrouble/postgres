#!/bin/bash
#-------------------------------------------------------------------------
# compare_connection_modes.sh
#   Compare performance of 4 connection modes via Unix socket (read-only test).
#   - PostgreSQL socket: /tmp
#   - pgBouncer socket: /var/run/postgresql
#
# Usage:
#   ./compare_connection_modes.sh [options]
#
# Options:
#   --clients N          Number of concurrent clients (default: 20)
#   --duration N         Test duration in seconds (default: 60)
#   --scale N            Scale factor (default: 1)
#   --pgbouncer-port PORT  pgBouncer Unix socket port (default: 6432)
#   --skip-mode MODE     Skip a mode (1,2,3,4 or "all")
#   --help               Show this help
#-------------------------------------------------------------------------

set -e

# Default values
CLIENTS=20
DURATION=60
SCALE=1
PGBOUNCER_PORT=6432
PGUSER=${PGUSER:-postgres}
PGDATABASE=${PGDATABASE:-postgres}
RESULTS_DIR="./results_$(date +%Y%m%d_%H%M%S)"
SKIP_MODES=""

# Unix socket directories
PG_SOCKET_DIR="/tmp"
PGBOUNCER_SOCKET_DIR="/var/run/postgresql"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

show_help() {
    cat << EOF
Usage: $0 [options]

Options:
  --clients N          Number of concurrent clients (default: 20)
  --duration N         Test duration in seconds (default: 60)
  --scale N            Scale factor for pgbench (default: 1)
  --pgbouncer-port PORT  pgBouncer Unix socket port (default: 6432)
  --skip-mode MODE     Skip a specific mode (1,2,3,4, or "all")
  --help               Show this help

Modes:
  1: No pool, long connections (pgbench without -C) - via PostgreSQL socket
  2: No pool, short connections (pgbench with -C) - via PostgreSQL socket
  3: pgBouncer connection pool (short connections) - via pgBouncer socket
  4: Built-in connection pool (short connections) - via PostgreSQL socket

Socket paths:
  PostgreSQL: $PG_SOCKET_DIR
  pgBouncer:  $PGBOUNCER_SOCKET_DIR
EOF
}

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
        --help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Connection Mode Performance Comparison (Read-Only)"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Clients: $CLIENTS"
echo "  Duration: ${DURATION}s"
echo "  Scale: $SCALE"
echo "  Results directory: $RESULTS_DIR"
echo "  PostgreSQL socket: $PG_SOCKET_DIR"
echo "  pgBouncer socket:  $PGBOUNCER_SOCKET_DIR"
echo "  pgBouncer port: $PGBOUNCER_PORT"
echo ""
echo "Testing 4 modes sequentially:"
echo "  1. No pool, long connections"
echo "  2. No pool, short connections"
echo "  3. pgBouncer connection pool"
echo "  4. Built-in connection pool"
echo ""

# Dependency checks
for cmd in pgbench psql pg_ctl; do
    if ! command -v $cmd >/dev/null 2>&1; then
        echo -e "${RED}Error: $cmd not found in PATH${NC}"
        exit 1
    fi
done

# Test PostgreSQL connectivity
if ! psql -h "$PG_SOCKET_DIR" -p 5432 -U $PGUSER -d $PGDATABASE -c "SELECT 1;" >/dev/null 2>&1; then
    echo -e "${RED}Error: Cannot connect to PostgreSQL via Unix socket ($PG_SOCKET_DIR, port 5432)${NC}"
    exit 1
fi

# Initialize pgbench database (once, read-only does not modify data)
echo -e "${BLUE}Initializing pgbench database...${NC}"
pgbench -h "$PG_SOCKET_DIR" -p 5432 -U $PGUSER -i -s $SCALE $PGDATABASE >/dev/null 2>&1
echo "Done."
echo ""

# Helper functions
should_skip() {
    local mode=$1
    for skip in $SKIP_MODES; do
        if [ "$skip" = "$mode" ] || [ "$skip" = "all" ]; then
            return 0
        fi
    done
    return 1
}

restart_postgres() {
    echo "Restarting PostgreSQL..."
    if [ "$(whoami)" = "postgres" ]; then
        pg_ctl restart -D "$PGDATA" -l logfile
    else
        sudo -u postgres pg_ctl restart -D "$PGDATA" -l logfile
    fi
    sleep 3
    if ! psql -h "$PG_SOCKET_DIR" -p 5432 -U $PGUSER -d $PGDATABASE -c "SELECT 1;" >/dev/null 2>&1; then
        echo -e "${RED}Error: PostgreSQL failed to restart${NC}"
        exit 1
    fi
    echo "PostgreSQL restarted."
}

set_pool_enabled() {
    local enabled=$1
    echo "Setting enable_connection_pool = $enabled"
    psql -h "$PG_SOCKET_DIR" -p 5432 -U $PGUSER -d $PGDATABASE -c \
         "ALTER SYSTEM SET enable_connection_pool = $enabled;" >/dev/null 2>&1
    if [ "$enabled" = "true" ]; then
        psql -h "$PG_SOCKET_DIR" -p 5432 -U $PGUSER -d $PGDATABASE -c \
             "ALTER SYSTEM SET connection_pool_size = $CLIENTS;" >/dev/null 2>&1
    fi
    restart_postgres
}

start_pgbouncer() {
    echo "Starting pgBouncer via systemctl..."
    sudo systemctl start pgbouncer
    sleep 2
    if ! psql -h "$PGBOUNCER_SOCKET_DIR" -p $PGBOUNCER_PORT -U $PGUSER -d $PGDATABASE -c "SELECT 1;" >/dev/null 2>&1; then
        echo -e "${RED}Error: pgBouncer failed to start or not accepting connections${NC}"
        exit 1
    fi
    echo "pgBouncer started."
}

stop_pgbouncer() {
    echo "Stopping pgBouncer via systemctl..."
    sudo systemctl stop pgbouncer
    sleep 2
    echo "pgBouncer stopped."
}

run_pgbench() {
    local mode=$1
    local description=$2
    local pgbench_args=$3
    local socket_dir=$4
    local port=$5
    local output_file="$RESULTS_DIR/mode_${mode}.txt"
    
    echo -e "${GREEN}=== Mode $mode: $description ===${NC}"
    echo "Running pgbench (read-only with -S)..."
    echo "  Socket: $socket_dir, port: $port"
    echo "  pgbench args: $pgbench_args"
    
    pgbench -h "$socket_dir" -p "$port" -U $PGUSER \
            -c $CLIENTS -j $CLIENTS -T $DURATION -S -n -r \
            $pgbench_args $PGDATABASE > "$output_file" 2>&1
    
    local tps=$(grep "tps = " "$output_file" | grep -v "including" | awk '{print $3}')
    local latency=$(grep "latency average = " "$output_file" | awk '{print $4}')
    local latency_stddev=$(grep "latency stddev = " "$output_file" | awk '{print $4}')
    
    echo "Performance Results:"
    echo "  TPS: $tps"
    echo "  Latency (avg): $latency"
    echo "  Latency (stddev): $latency_stddev"
    echo ""
    
    echo "Mode $mode: $description" >> "$RESULTS_DIR/summary.txt"
    echo "  TPS: $tps" >> "$RESULTS_DIR/summary.txt"
    echo "  Latency (avg): $latency" >> "$RESULTS_DIR/summary.txt"
    echo "  Latency (stddev): $latency_stddev" >> "$RESULTS_DIR/summary.txt"
    echo "" >> "$RESULTS_DIR/summary.txt"
    
    sleep 2
}

# ========== Mode 1: No pool, long connections ==========
if ! should_skip 1; then
    set_pool_enabled false
    run_pgbench 1 "No pool, long connections" "" "$PG_SOCKET_DIR" 5432
fi

# ========== Mode 2: No pool, short connections ==========
if ! should_skip 2; then
    set_pool_enabled false
    run_pgbench 2 "No pool, short connections" "-C" "$PG_SOCKET_DIR" 5432
fi

# ========== Mode 3: pgBouncer ==========
if ! should_skip 3; then
    set_pool_enabled false
    start_pgbouncer
    run_pgbench 3 "pgBouncer connection pool" "-C" "$PGBOUNCER_SOCKET_DIR" $PGBOUNCER_PORT
    stop_pgbouncer
fi

# ========== Mode 4: Built-in connection pool ==========
if ! should_skip 4; then
    set_pool_enabled true
    run_pgbench 4 "Built-in connection pool" "-C" "$PG_SOCKET_DIR" 5432
fi

# ========== Final report ==========
echo ""
echo "=========================================="
echo -e "${GREEN}All tests completed. Generating report...${NC}"
echo "=========================================="
echo ""

cat > "$RESULTS_DIR/comparison.txt" <<EOF
Connection Mode Performance Comparison (Read-Only)
==================================================

Test Configuration:
  Clients: $CLIENTS
  Duration: ${DURATION}s
  Scale: $SCALE
  Test Date: $(date)
  PostgreSQL socket: $PG_SOCKET_DIR
  pgBouncer socket:  $PGBOUNCER_SOCKET_DIR

Results Summary:
EOF

cat "$RESULTS_DIR/summary.txt" >> "$RESULTS_DIR/comparison.txt"

cat >> "$RESULTS_DIR/comparison.txt" <<EOF

Expected results:
  Mode 1 (long conn):        Highest TPS, lowest latency
  Mode 2 (short conn):       Lowest TPS, highest latency
  Mode 3 (pgBouncer):        Good TPS, low latency
  Mode 4 (built-in pool):    Should be close to Mode 1, better than Mode 2

Interpretation:
  - TPS: higher is better
  - Latency average: lower is better
  - Latency stddev: lower is more consistent

Detailed logs:
  Mode 1: $RESULTS_DIR/mode_1.txt
  Mode 2: $RESULTS_DIR/mode_2.txt
  Mode 3: $RESULTS_DIR/mode_3.txt
  Mode 4: $RESULTS_DIR/mode_4.txt
EOF

cat "$RESULTS_DIR/comparison.txt"

echo ""
echo -e "${GREEN}Results saved to: $RESULTS_DIR${NC}"
echo "To view summary: cat $RESULTS_DIR/comparison.txt"