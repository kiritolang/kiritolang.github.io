# Doc-vs-implementation discrepancies & silent bugs found during the verification pass

Format per entry:

```
### <area>-<n>: <one-line summary>
- kind: doc-missing | doc-wrong | silent-bug | wrong-message | crash | other
- location: docs/pages/<page>.md and/or src/kirito/<file>.hpp:<line>
- expected (per docs): ...
- actual: ...
- resolution: doc updated / test pins current behaviour / src fix flagged (needs approval)
```

(Empty so far — populated as agents run.)
