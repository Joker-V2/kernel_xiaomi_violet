#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

DEFINE_PER_CPU(struct ida_bitmap *, ida_bitmap);

int idr_alloc_cmn(struct idr *idr, void *ptr, unsigned long *index,
		  unsigned long start, unsigned long end, gfp_t gfp,
		  bool ext)
{
	struct radix_tree_iter iter;
	void __rcu **slot;

	if (WARN_ON_ONCE(radix_tree_is_internal_node(ptr)))
		return -EINVAL;

	radix_tree_iter_init(&iter, start);
	if (ext)
		slot = idr_get_free_ext(&idr->idr_rt, &iter, gfp, end);
	else
		slot = idr_get_free(&idr->idr_rt, &iter, gfp, end);
	if (IS_ERR(slot))
		return PTR_ERR(slot);

	radix_tree_iter_replace(&idr->idr_rt, &iter, slot, ptr);
	radix_tree_iter_tag_clear(&idr->idr_rt, &iter, IDR_FREE);

	if (index)
		*index = iter.index;
	return 0;
}
EXPORT_SYMBOL_GPL(idr_alloc_cmn);

int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp)
{
	int id, curr = idr->idr_next;

	if (curr < start)
		curr = start;

	id = idr_alloc(idr, ptr, curr, end, gfp);
	if ((id == -ENOSPC) && (curr > start))
		id = idr_alloc(idr, ptr, start, curr, gfp);

	if (id >= 0)
		idr->idr_next = id + 1U;

	return id;
}
EXPORT_SYMBOL(idr_alloc_cyclic);

int idr_for_each(const struct idr *idr,
		int (*fn)(int id, void *p, void *data), void *data)
{
	struct radix_tree_iter iter;
	void __rcu **slot;

	radix_tree_for_each_slot(slot, &idr->idr_rt, &iter, 0) {
		int ret = fn(iter.index, rcu_dereference_raw(*slot), data);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(idr_for_each);

void *idr_get_next(struct idr *idr, int *nextid)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	void *entry = NULL;

	radix_tree_for_each_slot(slot, &idr->idr_rt, &iter, *nextid) {
		entry = rcu_dereference_raw(*slot);
		if (!entry)
			continue;
		if (!radix_tree_deref_retry(entry))
			break;
		if (slot != (void *)&idr->idr_rt.rnode &&
				entry != (void *)RADIX_TREE_INTERNAL_NODE)
			break;
		slot = radix_tree_iter_retry(&iter);
	}
	if (!slot)
		return NULL;

	if (WARN_ON_ONCE(iter.index > INT_MAX))
		return NULL;

	*nextid = iter.index;
	return entry;
}
EXPORT_SYMBOL(idr_get_next);

void *idr_get_next_ext(struct idr *idr, unsigned long *nextid)
{
	struct radix_tree_iter iter;
	void __rcu **slot;

	slot = radix_tree_iter_find(&idr->idr_rt, &iter, *nextid);
	if (!slot)
		return NULL;

	*nextid = iter.index;
	return rcu_dereference_raw(*slot);
}
EXPORT_SYMBOL(idr_get_next_ext);

void *idr_get_next_ul(struct idr *idr, unsigned long *nextid)
    __attribute__((alias("idr_get_next_ext")));
EXPORT_SYMBOL(idr_get_next_ul);

void *idr_replace(struct idr *idr, void *ptr, int id)
{
	if (id < 0)
		return ERR_PTR(-EINVAL);

	return idr_replace_ext(idr, ptr, id);
}
EXPORT_SYMBOL(idr_replace);

void *idr_replace_ext(struct idr *idr, void *ptr, unsigned long id)
{
	struct radix_tree_node *node;
	void __rcu **slot = NULL;
	void *entry;

	if (WARN_ON_ONCE(radix_tree_is_internal_node(ptr)))
		return ERR_PTR(-EINVAL);

	entry = __radix_tree_lookup(&idr->idr_rt, id, &node, &slot);
	if (!slot || radix_tree_tag_get(&idr->idr_rt, id, IDR_FREE))
		return ERR_PTR(-ENOENT);

	__radix_tree_replace(&idr->idr_rt, node, slot, ptr, NULL, NULL);

	return entry;
}
EXPORT_SYMBOL(idr_replace_ext);

#define IDA_MAX (0x80000000U / IDA_BITMAP_BITS)

int ida_get_new_above(struct ida *ida, int start, int *id)
{
	struct radix_tree_root *root = &ida->ida_rt;
	void __rcu **slot;
	struct radix_tree_iter iter;
	struct ida_bitmap *bitmap;
	unsigned long index;
	unsigned bit, ebit;
	int new;

	index = start / IDA_BITMAP_BITS;
	bit = start % IDA_BITMAP_BITS;
	ebit = bit + RADIX_TREE_EXCEPTIONAL_SHIFT;

	slot = radix_tree_iter_init(&iter, index);
	for (;;) {
		if (slot)
			slot = radix_tree_next_slot(slot, &iter,
						RADIX_TREE_ITER_TAGGED);
		if (!slot) {
			slot = idr_get_free(root, &iter, GFP_NOWAIT, IDA_MAX);
			if (IS_ERR(slot)) {
				if (slot == ERR_PTR(-ENOMEM))
					return -EAGAIN;
				return PTR_ERR(slot);
			}
		}
		if (iter.index > index) {
			bit = 0;
			ebit = RADIX_TREE_EXCEPTIONAL_SHIFT;
		}
		new = iter.index * IDA_BITMAP_BITS;
		bitmap = rcu_dereference_raw(*slot);
		if (radix_tree_exception(bitmap)) {
			unsigned long tmp = (unsigned long)bitmap;
			ebit = find_next_zero_bit(&tmp, BITS_PER_LONG, ebit);
			if (ebit < BITS_PER_LONG) {
				tmp |= 1UL << ebit;
				rcu_assign_pointer(*slot, (void *)tmp);
				*id = new + ebit - RADIX_TREE_EXCEPTIONAL_SHIFT;
				return 0;
			}
			bitmap = this_cpu_xchg(ida_bitmap, NULL);
			if (!bitmap)
				return -EAGAIN;
			memset(bitmap, 0, sizeof(*bitmap));
			bitmap->bitmap[0] = tmp >> RADIX_TREE_EXCEPTIONAL_SHIFT;
			rcu_assign_pointer(*slot, bitmap);
		}

		if (bitmap) {
			bit = find_next_zero_bit(bitmap->bitmap,
							IDA_BITMAP_BITS, bit);
			new += bit;
			if (new < 0)
				return -ENOSPC;
			if (bit == IDA_BITMAP_BITS)
				continue;

			__set_bit(bit, bitmap->bitmap);
			if (bitmap_full(bitmap->bitmap, IDA_BITMAP_BITS))
				radix_tree_iter_tag_clear(root, &iter,
								IDR_FREE);
		} else {
			new += bit;
			if (new < 0)
				return -ENOSPC;
			if (ebit < BITS_PER_LONG) {
				bitmap = (void *)((1UL << ebit) |
						RADIX_TREE_EXCEPTIONAL_ENTRY);
				radix_tree_iter_replace(root, &iter, slot,
						bitmap);
				*id = new;
				return 0;
			}
			bitmap = this_cpu_xchg(ida_bitmap, NULL);
			if (!bitmap)
				return -EAGAIN;
			memset(bitmap, 0, sizeof(*bitmap));
			__set_bit(bit, bitmap->bitmap);
			radix_tree_iter_replace(root, &iter, slot, bitmap);
		}

		*id = new;
		return 0;
	}
}
EXPORT_SYMBOL(ida_get_new_above);

void ida_remove(struct ida *ida, int id)
{
	unsigned long index = id / IDA_BITMAP_BITS;
	unsigned offset = id % IDA_BITMAP_BITS;
	struct ida_bitmap *bitmap;
	unsigned long *btmp;
	struct radix_tree_iter iter;
	void __rcu **slot;

	slot = radix_tree_iter_lookup(&ida->ida_rt, &iter, index);
	if (!slot)
		goto err;

	bitmap = rcu_dereference_raw(*slot);
	if (radix_tree_exception(bitmap)) {
		btmp = (unsigned long *)slot;
		offset += RADIX_TREE_EXCEPTIONAL_SHIFT;
		if (offset >= BITS_PER_LONG)
			goto err;
	} else {
		btmp = bitmap->bitmap;
	}
	if (!bitmap || !test_bit(offset, btmp))
		goto err;

	__clear_bit(offset, btmp);
	radix_tree_iter_tag_set(&ida->ida_rt, &iter, IDR_FREE);
	if (radix_tree_exception(bitmap)) {
		if (rcu_dereference_raw(*slot) ==
					(void *)RADIX_TREE_EXCEPTIONAL_ENTRY)
			radix_tree_iter_delete(&ida->ida_rt, &iter, slot);
	} else if (bitmap_empty(btmp, IDA_BITMAP_BITS)) {
		kfree(bitmap);
		radix_tree_iter_delete(&ida->ida_rt, &iter, slot);
	}
	return;
 err:
	WARN(1, "ida_remove called for id=%d which is not allocated.\n", id);
}
EXPORT_SYMBOL(ida_remove);

void ida_destroy(struct ida *ida)
{
	unsigned long flags;
	struct radix_tree_iter iter;
	void __rcu **slot;

	xa_lock_irqsave(&ida->ida_rt, flags);
	radix_tree_for_each_slot(slot, &ida->ida_rt, &iter, 0) {
		struct ida_bitmap *bitmap = rcu_dereference_raw(*slot);
		if (!radix_tree_exception(bitmap))
			kfree(bitmap);
		radix_tree_iter_delete(&ida->ida_rt, &iter, slot);
	}
	xa_unlock_irqrestore(&ida->ida_rt, flags);
}
EXPORT_SYMBOL(ida_destroy);

int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max,
			gfp_t gfp)
{
	int ret, id;
	unsigned long flags;

	if ((int)min < 0)
		return -ENOSPC;

	if ((int)max < 0)
		max = INT_MAX;

again:
	xa_lock_irqsave(&ida->ida_rt, flags);
	ret = ida_get_new_above(ida, min, &id);
	if (!ret) {
		if (id > max) {
			ida_remove(ida, id);
			ret = -ENOSPC;
		} else {
			ret = id;
		}
	}
	xa_unlock_irqrestore(&ida->ida_rt, flags);

	if (unlikely(ret == -EAGAIN)) {
		if (!ida_pre_get(ida, gfp))
			return -ENOMEM;
		goto again;
	}

	return ret;
}
EXPORT_SYMBOL(ida_alloc_range);

void ida_free(struct ida *ida, unsigned int id)
{
	unsigned long flags;

	if ((int)id < 0)
		return;

	xa_lock_irqsave(&ida->ida_rt, flags);
	ida_remove(ida, id);
	xa_unlock_irqrestore(&ida->ida_rt, flags);
}
EXPORT_SYMBOL(ida_free);
