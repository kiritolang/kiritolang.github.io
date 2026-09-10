-- Cross-language benchmark -- Lua 5.1 side. `lua bench.lua <workload>` runs one workload and prints
-- its time in milliseconds (os.clock = CPU time; single-threaded CPU-bound, ~= wall time). Mirrors
-- bench.ki / bench.py (same algorithm + iteration counts).
local wl = arg[1]

local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

local function ack(m, n)
  if m == 0 then return n + 1 end
  if n == 0 then return ack(m - 1, 1) end
  return ack(m - 1, ack(m, n - 1))
end

local function gcd(a, b)
  while b ~= 0 do a, b = b, a % b end
  return a
end

local Vec = {}
Vec.__index = Vec
function Vec.new(x, y) return setmetatable({x = x, y = y}, Vec) end
function Vec:norm2() return self.x * self.x + self.y * self.y end

local Sq = {}
Sq.__index = Sq
function Sq.new(s) return setmetatable({s = s}, Sq) end
function Sq:area() return self.s * self.s end
local Ci = {}
Ci.__index = Ci
function Ci.new(r) return setmetatable({r = r}, Ci) end
function Ci:area() return 3 * self.r * self.r end

local function concat(a, b)
  local r = {}
  for i = 1, #a do r[#r + 1] = a[i] end
  for i = 1, #b do r[#r + 1] = b[i] end
  return r
end
local function qsort(a)
  if #a < 2 then return a end
  local pivot = a[math.floor(#a / 2) + 1]
  local lo, eq, hi = {}, {}, {}
  for i = 1, #a do
    local x = a[i]
    if x < pivot then lo[#lo + 1] = x
    elseif x > pivot then hi[#hi + 1] = x
    else eq[#eq + 1] = x end
  end
  return concat(concat(qsort(lo), eq), qsort(hi))
end

local function split(s, sep)
  local parts = {}
  for tok in string.gmatch(s, "([^" .. sep .. "]+)") do parts[#parts + 1] = tok end
  return parts
end

local function run(wl)
  if wl == "fib" then
    return fib(30)
  elseif wl == "ackermann" then
    return ack(3, 6)
  elseif wl == "sum_loop" then
    local s = 0
    for rep = 1, 2000 do
      local i = 0
      while i < 1000 do s = s + i; i = i + 1 end
    end
    return s
  elseif wl == "float_loop" then
    local s = 0.0
    for rep = 1, 2000 do
      local i = 0
      while i < 1000 do s = s + i * 0.5; i = i + 1 end
    end
    return math.floor(s % 1000.0)
  elseif wl == "nested_loop" then
    local s = 0
    local i = 0
    while i < 700 do
      local j = 0
      while j < 700 do s = s + i * j; j = j + 1 end
      i = i + 1
    end
    return s
  elseif wl == "sieve" then
    local s = 0
    for rep = 1, 300 do
      local sieve = {}
      local k = 0
      while k < 5000 do sieve[k] = true; k = k + 1 end
      local p = 2
      while p * p < 5000 do
        if sieve[p] then
          local m = p * p
          while m < 5000 do sieve[m] = false; m = m + p end
        end
        p = p + 1
      end
      local c = 0
      local q = 2
      while q < 5000 do if sieve[q] then c = c + 1 end; q = q + 1 end
      s = c
    end
    return s
  elseif wl == "collatz" then
    local total = 0
    local n = 1
    while n < 30000 do
      local x = n
      local steps = 0
      while x ~= 1 do
        if x % 2 == 0 then x = math.floor(x / 2) else x = 3 * x + 1 end
        steps = steps + 1
      end
      total = total + steps
      n = n + 1
    end
    return total
  elseif wl == "gcd_loop" then
    local s = 0
    local i = 1
    while i < 200000 do s = s + gcd(i, 12345); i = i + 1 end
    return s
  elseif wl == "list_build" then
    local n = 0
    for rep = 1, 2000 do
      local a = {}
      local i = 0
      while i < 1000 do a[#a + 1] = i; i = i + 1 end
      n = #a
    end
    return n
  elseif wl == "list_sum" then
    local a = {}
    local i = 0
    while i < 10000 do a[#a + 1] = i; i = i + 1 end
    local s = 0
    for rep = 1, 2000 do for j = 1, #a do s = s + a[j] end end
    return s
  elseif wl == "list_sort" then
    local s = 0
    for rep = 1, 2000 do
      local a = {}
      local i = 0
      local seed = 12345
      while i < 1000 do
        seed = (seed * 1103515245 + 12345) % 2147483648
        a[#a + 1] = seed % 100000
        i = i + 1
      end
      table.sort(a)
      s = a[1]
    end
    return s
  elseif wl == "quicksort" then
    local s = 0
    for rep = 1, 300 do
      local a = {}
      local i = 0
      local seed = 999
      while i < 1000 do
        seed = (seed * 1103515245 + 12345) % 2147483648
        a[#a + 1] = seed % 100000
        i = i + 1
      end
      local sortd = qsort(a)
      s = sortd[1]
    end
    return s
  elseif wl == "dict_build" then
    local n = 0
    for rep = 1, 1000 do
      local d = {}
      local i = 0
      while i < 2000 do d[i] = i * 2; i = i + 1 end
      local cnt = 0
      for _ in pairs(d) do cnt = cnt + 1 end
      n = cnt
    end
    return n
  elseif wl == "dict_lookup_int" then
    local d = {}
    local i = 0
    while i < 5000 do d[i] = i; i = i + 1 end
    local s = 0
    for rep = 1, 300 do
      local k = 0
      while k < 5000 do s = s + d[k]; k = k + 1 end
    end
    return s
  elseif wl == "dict_lookup_str" then
    local d = {}
    local i = 0
    while i < 5000 do d["key" .. i] = i; i = i + 1 end
    local s = 0
    for rep = 1, 300 do
      local k = 0
      while k < 5000 do s = s + d["key" .. k]; k = k + 1 end
    end
    return s
  elseif wl == "set_ops" then
    local s = 0
    for rep = 1, 2000 do
      local a = {}
      local i = 0
      while i < 500 do a[i] = true; i = i + 1 end
      local hits = 0
      local k = 0
      while k < 1000 do if a[k] then hits = hits + 1 end; k = k + 1 end
      s = hits
    end
    return s
  elseif wl == "str_concat" then
    local n = 0
    for rep = 1, 3000 do
      local s = ""
      local i = 0
      while i < 300 do s = s .. "x"; i = i + 1 end
      n = #s
    end
    return n
  elseif wl == "str_split_join" then
    local text = ""
    local i = 0
    while i < 2000 do text = text .. "word" .. i .. " "; i = i + 1 end
    local n = 0
    for rep = 1, 500 do
      local parts = split(text, " ")
      local joined = table.concat(parts, "-")
      n = #joined
    end
    return n
  elseif wl == "str_search" then
    local text = ""
    local i = 0
    while i < 3000 do text = text .. "abcdefg" .. (i % 10); i = i + 1 end
    local c = 0
    for rep = 1, 1000 do
      c = 0
      for _ in string.gmatch(text, "5") do c = c + 1 end
    end
    return c
  elseif wl == "method_call" then
    local v = Vec.new(3, 4)
    local s = 0
    local i = 0
    while i < 500000 do s = s + v:norm2(); i = i + 1 end
    return s
  elseif wl == "attr_rw" then
    local v = Vec.new(0, 0)
    local i = 0
    while i < 500000 do v.x = v.x + 1; v.y = v.x + v.y; i = i + 1 end
    return v.y
  elseif wl == "object_create" then
    local s = 0
    local i = 0
    while i < 300000 do local v = Vec.new(i, i + 1); s = s + v.x; i = i + 1 end
    return s
  elseif wl == "poly_dispatch" then
    local shapes = {}
    local i = 0
    while i < 1000 do
      if i % 2 == 0 then shapes[#shapes + 1] = Sq.new(i) else shapes[#shapes + 1] = Ci.new(i) end
      i = i + 1
    end
    local s = 0
    for rep = 1, 300 do
      for j = 1, #shapes do s = s + shapes[j]:area() end
    end
    return s
  elseif wl == "map_filter" then
    local a = {}
    local i = 0
    while i < 2000 do a[#a + 1] = i; i = i + 1 end
    local n = 0
    for rep = 1, 100 do
      local out = {}
      for j = 1, #a do
        local sq = a[j] * a[j]
        if sq % 2 == 0 then out[#out + 1] = sq end
      end
      n = #out
    end
    return n
  elseif wl == "matmul_manual" then
    local N = 40
    local A, B = {}, {}
    local i = 0
    while i < N do
      local ra, rb = {}, {}
      local j = 0
      while j < N do ra[#ra + 1] = i + j; rb[#rb + 1] = i - j; j = j + 1 end
      A[#A + 1] = ra; B[#B + 1] = rb
      i = i + 1
    end
    local s = 0
    for rep = 1, 30 do
      local C = {}
      for r = 1, N do
        local row = {}
        for c = 1, N do
          local acc = 0
          for k = 1, N do acc = acc + A[r][k] * B[k][c] end
          row[#row + 1] = acc
        end
        C[#C + 1] = row
      end
      s = C[1][1]
    end
    return s
  end
  return 0
end

local t0 = os.clock()
local _ = run(wl)
io.write(string.format("%.3f\n", (os.clock() - t0) * 1000.0))
