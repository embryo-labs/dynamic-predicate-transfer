-- Run after loading rpt. Look for PhysicalRPTCreateBF and PhysicalRPTUseBF.
CREATE TABLE a AS SELECT i AS k, i % 2  AS flag FROM range(1, 1000001) t(i);
CREATE TABLE b AS SELECT i AS k, i % 7  AS flag FROM range(1, 1000001) t(i);
CREATE TABLE c AS SELECT i AS k, i % 13 AS flag FROM range(1, 1000001) t(i);

EXPLAIN ANALYZE
SELECT count(*)
FROM a JOIN b USING (k) JOIN c USING (k)
WHERE a.flag = 0 AND b.flag = 0 AND c.flag = 0;
