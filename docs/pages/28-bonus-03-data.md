# Bonus Lesson 3 — Tabular Data Analysis

When you have *tabular* data — rows and columns, the shape of a CSV file — looping over lists of
dicts gets tedious fast. Kirito ships a **dataframe-style** library (`import("tabular")`) built on the
low-level [`csv`](stdlib.html#csv) module, with two core types: a 1-D labelled **`Series`** (one
column) and a 2-D **`DataFrame`** (a whole table). This lesson is a practical tour.

## A Series is a labelled column

```kirito
var io = import("io")
var tb = import("tabular")

var s = tb.Series([10, 20, 30, 40])
io.print(s.sum())          # 100
io.print(s.mean())         # 25.0
io.print(s.max())          # 40
io.print((s * 2).tolist()) # [20, 40, 60, 80]   — element-wise arithmetic
```

Comparisons produce a **boolean Series**, which is the key to filtering tables:

```kirito
var io = import("io")
var tb = import("tabular")
var s = tb.Series([10, 20, 30, 40])
io.print((s > 20).tolist())    # [False, False, True, True]
io.print((s == 20).tolist())   # [False, True, False, False]  — ==/!= are element-wise too
```

## Building a DataFrame

You can build a table from a Dict of columns, a list of rows (with `columns=`), or — most often —
by reading CSV. (Because Kirito dicts aren't ordered, pass `columns=` when you care about column
order; `readcsv` preserves the header order for you.)

```kirito
var io = import("io")
var tb = import("tabular")

var df = tb.DataFrame(
    [["Ada", "eng", 120], ["Alan", "eng", 110], ["Grace", "ops", 95], ["Edsger", "ops", 130]],
    columns=["name", "dept", "salary"])
io.print(df.shape())       # [4, 3]   (rows, cols)
io.print(df.columns)       # ['name', 'dept', 'salary']
io.print(df)               # pretty-prints the table
```

## Reading CSV (with type inference)

`readcsv` accepts CSV **text** (or a filename). Each cell is inferred to Integer / Float / Bool /
String, and an empty cell becomes `None` (missing):

```kirito
var io = import("io")
var tb = import("tabular")

var csv = "name,dept,salary\nAda,eng,120\nAlan,eng,110\nGrace,ops,95\nEdsger,ops,130"
var df = tb.readcsv(csv)
io.print(type(df["salary"][0]))   # Integer  (inferred)
io.print(df["dept"].tolist())     # ['eng', 'eng', 'ops', 'ops']
```

## Selecting and filtering

Index a column by name; pass a list of names for a sub-table; pass a **boolean Series** to keep
matching rows. `df.iloc[i]` / `df.loc[label]` get a row.

```kirito
var io = import("io")
var tb = import("tabular")
var df = tb.readcsv("name,dept,salary\nAda,eng,120\nAlan,eng,110\nGrace,ops,95\nEdsger,ops,130")

io.print(df["salary"].mean())                       # 113.75
io.print(df[df["salary"] > 100]["name"].tolist())   # ['Ada', 'Alan', 'Edsger']   (boolean mask)
io.print(df[["name", "salary"]].columns)            # ['name', 'salary']          (column subset)
io.print(df.iloc[0]["name"])                        # Ada                    (first row)
```

## Group-by and aggregation — the workhorse

`groupby(col)` splits the table by a key column; then `sum`/`mean`/`min`/`max`/`std`/`count` reduce
each numeric column per group, or `agg({col: "reduction"})` picks per-column reductions.

```kirito
var io = import("io")
var tb = import("tabular")
var df = tb.readcsv("name,dept,salary\nAda,eng,120\nAlan,eng,110\nGrace,ops,95\nEdsger,ops,130")

io.print(df.groupby("dept").mean())                 # mean salary per dept
io.print(df.groupby("dept").agg({"salary": "max"})) # top salary per dept
```

prints (eng then ops, in first-seen order):

```text
       salary
eng     115.0
ops     112.5
       salary
eng       120
ops       130
```

## Sorting, new columns, and `describe`

Assigning a Series (or a list, or a scalar) adds/replaces a column. `sortvalues` orders rows;
`describe` gives count/mean/std/min/median/max for every numeric column.

```kirito
var io = import("io")
var tb = import("tabular")
var df = tb.readcsv("name,dept,salary\nAda,eng,120\nAlan,eng,110\nGrace,ops,95\nEdsger,ops,130")

df["bonus"] = df["salary"] * 0.1                    # derived column
io.print(df.sortvalues("salary", ascending=False)["name"].tolist())  # ['Edsger', 'Ada', 'Alan', 'Grace']
io.print(df.describe())                             # numeric summary
```

## Missing data

Real data has gaps. An empty CSV cell is `None`; numeric aggregations skip it, `count()` tallies the
non-null values (of any type), and `dropna`/`fillna` clean it up.

```kirito
var io = import("io")
var tb = import("tabular")

var df = tb.readcsv("name,age\nAda,36\nBjarne,\nGrace,85")
io.print(df["age"].count())          # 2   (the blank is skipped)
io.print(df["age"].mean())           # 60.5
io.print(df.dropna()["name"].tolist())          # ['Ada', 'Grace']
io.print(df.fillna(0)["age"].tolist())          # [36, 0, 85]
```

## Joining and stacking

`merge` joins two tables on a key column (`how` = `"inner"`/`"left"`/`"right"`/`"outer"`); `concat`
stacks tables vertically.

```kirito
var io = import("io")
var tb = import("tabular")

var orders = tb.DataFrame([[1, 2], [2, 3]], columns=["id", "product_id"])
var products = tb.DataFrame([[2, "Mouse"], [3, "Monitor"]], columns=["product_id", "name"])
var joined = tb.merge(orders, products, on="product_id")
io.print(joined["name"].tolist())    # ['Mouse', 'Monitor']
```

## Putting it together

The `examples/` directory has three complete programs built on this library:
[`tabular_iris.ki`](https://github.com/kiritolang/kiritolang.github.io/blob/main/examples/tabular_iris.ki) (a
full exploratory analysis of the iris dataset — group means, a derived feature, a correlation),
[`tabular_sales.ki`](https://github.com/kiritolang/kiritolang.github.io/blob/main/examples/tabular_sales.ki) (a
generated sales dataset joined to a catalog, then revenue ranked by region and product), and
[`tabular_survey.ki`](https://github.com/kiritolang/kiritolang.github.io/blob/main/examples/tabular_survey.ki) (a
messy survey CSV cleaned with `dropna`/`fillna`/`apply`/`astype`).

## What you learned

- **`Series`** (a labelled column) supports element-wise arithmetic and comparisons; a comparison
  yields a boolean Series for masking.
- **`DataFrame`** is a table you build from columns, rows, or `readcsv` (with type inference);
  select a column with `df["c"]`, a subset with `df[["a", "b"]]`, and matching rows with a boolean
  mask.
- **`groupby(col).agg(...)`** is the analysis workhorse; `sortvalues`, derived columns, and
  `describe` round out the toolkit, and `dropna`/`fillna` handle missing data.
- For the full method list, see the **Standard Library** reference's
  [`tabular`](stdlib.html#tabular) section.

Next bonus lesson: **linear algebra** — matrices, vectors, and complex numbers.
