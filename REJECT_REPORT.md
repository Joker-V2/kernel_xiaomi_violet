# Patch Reject Report
## LG Kernel → Xiaomi Kernel

**Commit:** 7f2847d02cdc4491b5ee6d4a0043854cbd6c7a1a
**Date:** Mon Feb  9 21:59:55 UTC 2026
**Branch:** zyc2

## Summary
- Applied: 6 hunks
- Rejected: 1 hunks

## Files with Rejects

### task_mmu.c
- **File:** `./fs/proc/task_mmu.c`
- **Reject:** `./fs/proc/task_mmu.c.rej`

**First 5 lines:**
```diff
diff a/fs/proc/task_mmu.c b/fs/proc/task_mmu.c	(rejected hunks)
@@ -26,6 +26,8 @@
 #include <asm/tlbflush.h>
 #include "internal.h"
 
```

## How to Fix
1. Open each `.rej` file
2. Compare with the original file
3. Apply changes manually
4. Delete `.rej` files after fixing
