# Patch Reject Report
## LG Kernel → Xiaomi Kernel

**Commit:** 95f8be4c8a86a491a1c2ac9bfe470aef9e1baa8f
**Date:** Mon Feb  9 23:03:38 UTC 2026
**Branch:** zyc2

## Summary
- Applied: 3 hunks
- Rejected: 1 hunks

## Files with Rejects

### task_mmu.c
- **File:** `./fs/proc/task_mmu.c`
- **Reject:** `./fs/proc/task_mmu.c.rej`

**First 5 lines:**
```diff
diff a/fs/proc/task_mmu.c b/fs/proc/task_mmu.c	(rejected hunks)
@@ -350,7 +350,7 @@ extern void susfs_sus_ino_for_show_map_vma(unsigned long ino, dev_t *out_dev, un
 #endif
 
 static void
```

## How to Fix
1. Open each `.rej` file
2. Compare with the original file
3. Apply changes manually
4. Delete `.rej` files after fixing
