-- Lua 5.1 baseline for the cross-language benchmark. Same workloads, same LCG, same protocol as
-- compare_bench.cpp / .py / .ki:
--     lua5.1 compare_bench.lua <workload> <N> <reps>   ->   prints "<mean_ns> <stddev_ns>"
--
-- Lua 5.1 numbers are IEEE doubles (no integer type, no bitwise ops), so the 31-bit LCG's
-- `state * 1103515245` (which reaches ~2^61) would lose precision past 2^53 and diverge from the
-- int64/bignum baselines. It is therefore evaluated with a 16-bit split-multiply that keeps every
-- intermediate exact below 2^53, reproducing the identical pseudo-random sequence. Timing uses
-- os.clock() (CPU seconds) — the highest-resolution clock stock Lua 5.1 offers.

local wl   = arg[1]
local N    = tonumber(arg[2])
local reps = tonumber(arg[3])

local lcg_state = 0
-- (state * 1103515245 + 12345) mod 2^31, exact via a 16-bit split of the multiplicand.
local function lcg_next()
  local s = lcg_state
  local sh = math.floor(s / 65536)          -- high 15 bits of state
  local sl = s - sh * 65536                 -- low 16 bits
  local a = 1103515245
  local t1 = ((sh * a) % 32768) * 65536     -- (sh*a mod 2^15) << 16   (< 2^31, exact)
  local t2 = (sl * a) % 2147483648          -- sl*a mod 2^31           (sl*a < 2^47, exact)
  lcg_state = (t1 + t2 + 12345) % 2147483648
  return lcg_state
end

local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

-- inputs for the optimistic workloads, built once (outside the timed loop)
local base, keys, text
if wl == "sort" then
  lcg_state = 12345
  base = {}
  for i = 1, N do base[i] = lcg_next() % 1000000 end
elseif wl == "dict_ops" then
  lcg_state = 777
  keys = {}
  for i = 1, N do keys[i] = lcg_next() end
elseif wl == "string_ops" then
  lcg_state = 99
  local parts = {}
  for i = 1, N do parts[i] = "w" .. (lcg_next() % 10000) end
  text = table.concat(parts, " ")
end

local function run_sum_loop()
  local s = 0
  for i = 0, N - 1 do s = s + i * 2 - i end
  return s
end

local function run_sieve()
  local sieve = {}
  for i = 0, N do sieve[i] = true end
  local count = 0
  local p = 2
  while p <= N do
    if sieve[p] then
      count = count + 1
      local m = p * p
      while m <= N do sieve[m] = false; m = m + p end
    end
    p = p + 1
  end
  return count
end

local function run_sort()
  local a = {}
  for i = 1, N do a[i] = base[i] end
  table.sort(a)
  return a[1] + a[N] + a[math.floor(N / 2) + 1]
end

local function run_dict_ops()
  local d = {}
  for i = 1, N do local k = keys[i]; d[k] = k * k end
  local sum = 0
  for i = 1, N do sum = sum + d[keys[i]] end
  return sum
end

local function run_string_ops()
  local words, n = {}, 0
  for w in string.gmatch(text, "[^ ]+") do n = n + 1; words[n] = w end
  local joined = table.concat(words, "-")
  return #joined + n
end

local function once()
  if wl == "sum_loop"   then return run_sum_loop() end
  if wl == "fib"        then return fib(N) end
  if wl == "sieve"      then return run_sieve() end
  if wl == "sort"       then return run_sort() end
  if wl == "dict_ops"   then return run_dict_ops() end
  if wl == "string_ops" then return run_string_ops() end
  io.stderr:write("unknown workload '" .. tostring(wl) .. "'\n")
  os.exit(2)
end

local sink = once()  -- warmup
local samples = {}
for r = 1, reps do
  local t0 = os.clock()
  sink = sink + once()
  local t1 = os.clock()
  samples[r] = (t1 - t0) * 1e9
end

local total = 0
for r = 1, reps do total = total + samples[r] end
local mean = total / reps
local sq = 0
for r = 1, reps do local d = samples[r] - mean; sq = sq + d * d end
local stddev = math.sqrt(sq / reps)
print(string.format("%.1f %.1f", mean, stddev))
io.stderr:write("checksum=" .. string.format("%.0f", sink) .. "\n")
