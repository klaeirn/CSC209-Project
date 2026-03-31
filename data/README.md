synthetic dataset for quick project testing.

format:
- each row is `x1,x2,x3,x4,y`
- `x1..x4` match `DEFAULT_MODEL_DIM` in `common.h`
- `y` is the regression target

notes:
- the data is available as `client_1.csv` through `client_12.csv` so you can run 12 clients concurrently.
- files `client_4..12.csv` reuse the same validated distribution pattern as `client_1..3.csv`.
- the targets were generated from a fixed linear rule, so this is useful for deterministic testing:

`y = 1.5*x1 - 2.0*x2 + 0.5*x3 + 3.0*x4 + 1.0`
