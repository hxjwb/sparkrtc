import json, math, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'archive/proace_20251120_102518/frame_delays.json'
with open(path) as f:
    data = json.load(f)

vals = sorted(x['e2e'] for x in data if 'e2e' in x)

def percentile_nearest_rank(values, p):
    n = len(values)
    k = math.ceil(p / 100.0 * n)
    k = max(1, min(k, n))
    return values[k - 1]

p95 = percentile_nearest_rank(vals, 95)
p99 = percentile_nearest_rank(vals, 99)

print(f'count={len(vals)} p95={p95} p99={p99}')