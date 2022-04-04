#!/usr/bin/env bash
# Tags: race, long

# This test is disabled because it triggers internal assert in Thread Sanitizer.
# Thread Sanitizer does not support for more than 64 mutexes to be locked in a single thread.
# https://github.com/google/sanitizers/issues/950

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

set -e

$CLICKHOUSE_CLIENT -nm -q "
    DROP TABLE IF EXISTS alter_table;
    CREATE TABLE alter_table (a UInt8, b Int16, c Float32, d String, e Array(UInt8), f Nullable(UUID), g Tuple(UInt8, UInt16)) ENGINE = MergeTree ORDER BY a PARTITION BY b % 10 SETTINGS old_parts_lifetime = 1;
"

function thread1()
{
    # NOTE: database = $CLICKHOUSE_DATABASE is unwanted
    $CLICKHOUSE_CLIENT --query "SELECT * FROM system.parts FORMAT Null"
}

function thread2()
{
    $CLICKHOUSE_CLIENT -n --query "ALTER TABLE alter_table ADD COLUMN h String '0'; ALTER TABLE alter_table MODIFY COLUMN h UInt64; ALTER TABLE alter_table DROP COLUMN h;"
}

function thread3()
{
    $CLICKHOUSE_CLIENT -q "INSERT INTO alter_table SELECT rand(1), rand(2), 1 / rand(3), toString(rand(4)), [rand(5), rand(6)], rand(7) % 2 ? NULL : generateUUIDv4(), (rand(8), rand(9)) FROM numbers(100000)"
}

function thread4()
{
    $CLICKHOUSE_CLIENT -q "OPTIMIZE TABLE alter_table FINAL"
}

function thread5()
{
    $CLICKHOUSE_CLIENT -q "ALTER TABLE alter_table DELETE WHERE rand() % 2 = 1"
}

export -f thread1
export -f thread2
export -f thread3
export -f thread4
export -f thread5

TIMEOUT=30

clickhouse_client_loop_timeout $TIMEOUT thread1 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread2 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread3 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread4 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread5 2> /dev/null &

clickhouse_client_loop_timeout $TIMEOUT thread1 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread2 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread3 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread4 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread5 2> /dev/null &

clickhouse_client_loop_timeout $TIMEOUT thread1 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread2 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread3 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread4 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread5 2> /dev/null &

clickhouse_client_loop_timeout $TIMEOUT thread1 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread2 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread3 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread4 2> /dev/null &
clickhouse_client_loop_timeout $TIMEOUT thread5 2> /dev/null &


wait

$CLICKHOUSE_CLIENT -q "DROP TABLE alter_table"
