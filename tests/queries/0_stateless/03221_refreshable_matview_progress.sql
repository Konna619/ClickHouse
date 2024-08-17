-- Tags: no-ordinary-database

set allow_experimental_refreshable_materialized_view=1;

CREATE MATERIALIZED VIEW 03221_rmv
REFRESH AFTER 1 SECOND
(
x UInt64
)
ENGINE = Memory
AS SELECT number AS x
FROM numbers(3)
UNION ALL
SELECT rand64() AS x;

SELECT sleep(2);

SELECT read_rows, total_rows, progress FROM system.view_refreshes WHERE view = '03221_rmv';

DROP TABLE 03221_rmv;
