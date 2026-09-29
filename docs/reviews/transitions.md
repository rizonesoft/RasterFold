# Outcome transitions

One block per finding whose disposition is refuted, withdrawn, duplicate, or routed: final states deserve a sentence, but a finding that ends anywhere other than fixed or filed keeps its when, why, and evidence, quoted from the record that decided it. Each block carries `as-of:`, the commit whose tree holds the quoted record. Checked by `scripts/todo-findings.py --check`: every non-final ledger row has exactly one block, every block names a live row, the block's `to` agrees with the row's disposition, and the `as-of` resolves to a commit.

The block shape, one field per line, blocks separated by a blank line:

```
transition: DNN-TNN-SN-FN
date: YYYY-MM-DD
from: raised
to: refuted | withdrawn | duplicate | routed
why: "<quoted from the deciding record>"
evidence: "<quoted from the deciding record>"
as-of: <commit sha>
```

No transitions are recorded yet.
