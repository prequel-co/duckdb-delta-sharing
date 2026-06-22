---
name: visualize-sql
description: Generates Grammar of Graphics visualizations from Delta Sharing tables using DuckDB and ggsql. Use when the user asks to create charts, plots, or visual analytical summaries.
---

## Visualization Workflow
Follow this checklist when asked to visualize data:
- [ ] Step 1: Determine output mode preference for the session
- [ ] Step 2: Evaluate schema via DESCRIBE (unless geom is specified)
- [ ] Step 3: Write and execute the visual query applying visual design best practices

**Step 1: Determine Output Mode**
First visualization only: ask user for output preference (`silent`, `html`, `url`).
- **`silent`**: Local browser. Blocks Bash (DuckDB process stays alive).
- **`html`**: Returns HTML doc. Bash redirect robust (`> out.html`), user must open manually.
- **`url`**: Opens browser, returns URL.
Assume preference for the session. Prefix queries with `SET ggsql_output = '...';`

**Step 2: Evaluate Schema & Select Geom**
Run `DESCRIBE` on the Delta table. Use heuristics unless user specified a chart type.
- **Time/Trends (Continuous)**: `DRAW line`
- **Time/Trends (Discrete, e.g., quarters)**: `DRAW bar`
- **Distributions**: `DRAW histogram` | `DRAW density`
- **Relationships (Num+Num)**: `DRAW point` (Scatter)
- **Comparisons**: `DRAW bar` (sort highest-to-lowest unless natural order exists)
- **Part-to-Whole**: `DRAW bar` (stacked) or use donut
- **Intensity**: `DRAW heatmap`

**Step 3: Write & Execute Visual Query**
Combine SQL with `ggsql`. Apply the following mandatory visual design constraints. Do NOT attempt to look up external guidelines; rely entirely on these rules to maximize insight and minimize clutter:
- **Bar/Column**: MUST start numerical axis at zero. Wrap text instead of rotating labels. Use horizontal bars for long labels. Limit to Top 15-20 categories to prevent vertical clutter. Use a single color for all bars; reserve accent colors ONLY to highlight specific values.
- **Line**: Limit to 5-6 lines. Always `GROUP BY` your time and series columns (e.g. `SUM()`, `AVG()`); never plot raw transactional data as it creates unreadable zig-zags. Ensure consistent time intervals.
- **Pie/Donut**: Prefer donut. Max 3-5 slices (never > 6). Label slices directly (no legends). No 3D, shadows, or exploded slices. Must sum to 100%. If more than 5 categories exist, you MUST group the remainder into an "Other" slice to preserve the part-to-whole integrity.
- **Scatter/Bubble**: Consistent axes. Label notable outliers. NEVER plot > 5,000 points (use `USING SAMPLE 5000 ROWS` or switch to a heatmap/density plot) to prevent overplotting and browser crashes.
- **Heatmap**: Use sequential palettes for intensity. Only use diverging palettes if there is a meaningful midpoint.
- **Maps**: Use normalized values (rates or percentages) for choropleth maps, never raw counts.
- **Table vs Chart**: If comparing only 1-2 values, do not visualize; use a simple markdown table instead.
- **Cleanliness**: Reduce gridlines. Lighten axis lines. Avoid background clutter.
- **Scales**: Use `SCALE color TO [viridis|accent|...]`.
- **Labels**: `LABEL title '...', x '...', y '...'`. Keep labels concise.

### Examples

**1. Horizontal Bar Chart (Sorted, Clean, Top N)**
```sql
SELECT department, SUM(budget) AS total_budget 
FROM delta_share_read('<share>', '<schema>', '<table>')
GROUP BY department
ORDER BY total_budget DESC
LIMIT 15 -- Prevent vertical clutter
VISUALIZE total_budget AS x, department AS y
DRAW bar
SCALE fill TO accent -- Uses a single accent color
LABEL title 'Top 15 Departments by Budget', x 'Total Budget ($)', y '';
```

**2. Line Chart (Time Series, Aggregated, Limited Lines)**
```sql
-- MUST aggregate time series to avoid zig-zagging raw data
SELECT date_trunc('month', order_date) AS order_month, region, SUM(revenue) AS revenue
FROM delta_share_read('<share>', '<schema>', '<table>')
WHERE region IN ('North', 'South', 'East', 'West') -- Limit to < 6 lines
GROUP BY 1, 2
ORDER BY order_month ASC
VISUALIZE order_month AS x, revenue AS y, region AS color
DRAW line
SCALE color TO viridis
LABEL title 'Monthly Revenue by Region', x 'Month', y 'Revenue ($)';
```

**3. Donut Chart (Max 5 slices, Part-to-Whole)**
```sql
WITH Ranked AS (
  SELECT category, COUNT(*) as count,
         ROW_NUMBER() OVER (ORDER BY COUNT(*) DESC) as rn
  FROM delta_share_read('<share>', '<schema>', '<table>')
  GROUP BY category
)
SELECT 
  CASE WHEN rn <= 4 THEN category ELSE 'Other' END AS category_grouped,
  SUM(count) AS total_count
FROM Ranked
GROUP BY category_grouped
ORDER BY total_count DESC
VISUALIZE total_count AS theta, category_grouped AS color
DRAW donut
SCALE color TO accent
LABEL title 'Top Categories';
```

**4. Scatter Plot (Sampled to Prevent Overplotting)**
```sql
-- MUST sample large Delta tables to prevent browser crash from overplotting
SELECT age, income 
FROM delta_share_read('<share>', '<schema>', '<table>')
USING SAMPLE 5000 ROWS
VISUALIZE age AS x, income AS y
DRAW point
SCALE fill TO viridis
LABEL title 'Income vs. Age (Sampled)', x 'Age', y 'Income ($)';
```

## OS-Specific Execution

**Unix / macOS:**
```bash
duckdb :memory: -json <<'SQL'
INSTALL ggsql FROM community; LOAD ggsql;
SET ggsql_output = 'html';
SELECT ... VISUALIZE ...
SQL
```

**Windows (PowerShell):**
```powershell
$sql = @"
INSTALL ggsql FROM community; LOAD ggsql;
SET ggsql_output = 'html';
SELECT ... VISUALIZE ...
"@
$sql | duckdb :memory: -json
```
