# Patch Reject Report
## LG Kernel → Xiaomi Kernel

**Commit:** 27956d255e3b012372951dd6131e07c106d2daae
**Date:** Mon Feb  9 22:43:26 UTC 2026
**Branch:** zyc2

## Summary
- Applied: 2 hunks
- Rejected: 1 hunks

## Files with Rejects

### task_mmu.c
- **File:** `./fs/proc/task_mmu.c`
- **Reject:** `./fs/proc/task_mmu.c.rej`

**First 5 lines:**
```diff
diff a/fs/proc/task_mmu.c b/fs/proc/task_mmu.c	(rejected hunks)
@@ -331,15 +331,18 @@ static void show_vma_header_prefix(struct seq_file *m,
 				   dev_t dev, unsigned long ino)
 {
 	seq_setwidth(m, 25 + sizeof(void *) * 6 - 1);
```

## How to Fix
1. Open each `.rej` file
2. Compare with the original file
3. Apply changes manually
4. Delete `.rej` files after fixing
