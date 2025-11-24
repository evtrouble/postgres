#!/bin/bash
#-------------------------------------------------------------------------
# test_pgbench_pool.sh
#   Use pgbench to test connection pool performance
#
# This script uses pgbench to create realistic database load and measure
# the impact of connection pooling.
#
# Usage:
#   ./test_pgbench_pool.sh [options]
#
# Options:
#   --clients N      Number of concurrent clients (default: 10)
#   --transactions N Number of transactions per client (default: 1000)
#   --duration N     Duration in seconds (default: 60)
#   --scale N        Scale factor (default: 1)
#-------------------------------------------------------------------------

set -e

# Default values
CLIENTS=10
TRANSACTIONS=1000
DURATION=60
SCALE=1
PGHOST=${PGHOST:-localhost}
PGPORT=${PGPORT:-5432}
PGUSER=${PGUSER:-$USER}
PGDATABASE=${PGDATABASE:-postgres}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clients)
            CLIENTS="$2"
            shift 2
            ;;
        --transactions)
            TRANSACTIONS="$2"
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
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "=========================================="
echo "pgbench Connection Pool Test"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Clients: $CLIENTS"
echo "  Transactions: $TRANSACTIONS"
echo "  Duration: ${DURATION}s"
echo "  Scale: $SCALE"
echo ""

# Initialize pgbench database
echo "Initializing pgbench database..."
pgbench -h $PGHOST -p $PGPORT -U $PGUSER -i -s $SCALE $PGDATABASE

echo ""
echo "=========================================="
echo "Running pgbench test..."
echo "=========================================="
echo ""

# Run pgbench with detailed output
pgbench -h $PGHOST -p $PGPORT -U $PGUSER \
        -c $CLIENTS \
        -j $CLIENTS \
        -T $DURATION \
        -r \
        $PGDATABASE

echo ""
echo "=========================================="
echo "Key Metrics to Watch:"
echo "=========================================="
echo ""
echo "1. tps (transactions per second)"
echo "   - Higher is better"
echo "   - Connection pool should improve this by reducing connection overhead"
echo ""
echo "2. latency (average transaction latency)"
echo "   - Lower is better"
echo "   - Connection pool should reduce initial connection time"
echo ""
echo "3. Connection establishment time"
echo "   - Check PostgreSQL logs for connection timing"
echo ""
echo "To compare with/without pool:"
echo "  1. Set enable_connection_pool = false, run test, record results"
echo "  2. Set enable_connection_pool = true, run test, record results"
echo "  3. Compare tps and latency metrics"
echo ""

