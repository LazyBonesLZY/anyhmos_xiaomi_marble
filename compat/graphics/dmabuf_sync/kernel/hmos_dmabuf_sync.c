// SPDX-License-Identifier: GPL-2.0-only
#ifdef HMOS_SYNC_HOST_TEST
#include "bridge_host_shim.h"
#define hmos_dma_resv_get_fences dma_resv_get_fences
#else
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-resv.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
static inline int hmos_dma_resv_get_fences(struct dma_resv *obj,
	struct dma_fence **pfence_excl, unsigned int *pshared_count,
	struct dma_fence ***pshared)
{
	return dma_resv_get_fences_rcu(obj, pfence_excl, pshared_count, pshared);
}
#else
#define hmos_dma_resv_get_fences dma_resv_get_fences
#endif
#endif
#include "../uapi/hmos_dmabuf_sync.h"

#define HMOS_FENCE_LIMIT 4096U
#define HMOS_CONTEXT_COUNT 0xffffffffULL

struct hmos_fences {
	struct dma_fence **items;
	unsigned int count;
	unsigned int capacity;
};

static u64 hmos_context_base;
static atomic64_t hmos_context_next = ATOMIC64_INIT(0);

// 预留唯一上下文区间，外部数组不能被误判为自建数组
static int hmos_context_initialize(void)
{
	hmos_context_base = dma_fence_context_alloc((unsigned int)HMOS_CONTEXT_COUNT);
	if (hmos_context_base > (~0ULL - HMOS_CONTEXT_COUNT))
		return -EOVERFLOW;
	return 0;
}

static bool hmos_owned_array(struct dma_fence *fence)
{
	return fence && dma_fence_is_array(fence) &&
		fence->context >= hmos_context_base &&
		fence->context - hmos_context_base < HMOS_CONTEXT_COUNT;
}

static void hmos_put_fences(struct dma_fence **fences, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i)
		dma_fence_put(fences[i]);
	kfree(fences);
}

static int hmos_append_fence(struct hmos_fences *output,
	struct dma_fence *fence, bool keep_error)
{
	struct dma_fence **grown;
	unsigned int i, capacity;

	if (dma_fence_is_signaled(fence) && !(keep_error && fence->error < 0))
		return 0;
	for (i = 0; i < output->count; ++i) {
		// 独立栅栏可能随后失败，只合并同一个引用对象
		if (output->items[i] == fence)
			return 0;
	}
	if (output->count == HMOS_FENCE_LIMIT)
		return -E2BIG;
	if (output->count == output->capacity) {
		capacity = output->capacity ? output->capacity * 2 : 8;
		grown = krealloc(output->items, capacity * sizeof(*grown), GFP_KERNEL);
		if (!grown)
			return -ENOMEM;
		output->items = grown;
		output->capacity = capacity;
	}
	output->items[output->count++] = dma_fence_get(fence);
	return 0;
}

// 仅展平自建 AND，保留外部容器及其完成条件
static int hmos_collect_head(struct hmos_fences *output,
	struct dma_fence *head, bool keep_error)
{
	struct dma_fence_array *array;
	unsigned int i;
	int error;

	if (!hmos_owned_array(head))
		return hmos_append_fence(output, head, keep_error);
	if (dma_fence_is_signaled(head) && !keep_error)
		return 0;
	array = to_dma_fence_array(head);
	for (i = 0; i < array->num_fences; ++i) {
		if (hmos_owned_array(array->fences[i]))
			return -EUCLEAN;
		// 未完成集合中的已失败子项仍属于原等待结果
		error = hmos_append_fence(output, array->fences[i], true);
		if (error)
			return error;
	}
	return 0;
}

static struct dma_fence *hmos_merge_fences(struct dma_fence **heads,
	unsigned int count, struct dma_fence *extra)
{
	struct hmos_fences output = {0};
	struct dma_fence_array *array;
	struct dma_fence *result;
	u64 offset;
	unsigned int i;
	int error;

	for (i = 0; i < count; ++i) {
		error = hmos_collect_head(&output, heads[i], false);
		if (error)
			goto put_output;
	}
	if (extra) {
		error = hmos_collect_head(&output, extra, true);
		if (error)
			goto put_output;
	}
	if (!output.count) {
		kfree(output.items);
		return dma_fence_get_stub();
	}
	if (output.count == 1) {
		result = output.items[0];
		kfree(output.items);
		return result;
	}
	offset = atomic64_fetch_inc(&hmos_context_next);
	if (offset >= HMOS_CONTEXT_COUNT) {
		error = -EOVERFLOW;
		goto put_output;
	}
	array = dma_fence_array_create(output.count, output.items,
		hmos_context_base + offset, 1, false);
	if (!array) {
		error = -ENOMEM;
		goto put_output;
	}
	return &array->base;

put_output:
	hmos_put_fences(output.items, output.count);
	return ERR_PTR(error);
}

// 所有失败点通过后才挂回调，失败请求不留下异步引用
static void hmos_publish_fence(struct dma_fence *fence)
{
	if (hmos_owned_array(fence))
		dma_fence_enable_sw_signaling(fence);
}

// 读取等待写者，写入等待当前全部访问者
static struct dma_fence *hmos_snapshot(struct dma_resv *reservation, bool write)
{
	struct dma_fence **shared = NULL;
	struct dma_fence *exclusive = NULL, *result;
	unsigned int count = 0;
	int error;

	error = hmos_dma_resv_get_fences(reservation, write ? NULL : &exclusive,
		&count, &shared);
	if (error) {
		hmos_put_fences(shared, count);
		dma_fence_put(exclusive);
		return ERR_PTR(error);
	}
	if (!write) {
		hmos_put_fences(shared, count);
		return exclusive ? exclusive : dma_fence_get_stub();
	}
	result = hmos_merge_fences(shared, count, NULL);
	hmos_put_fences(shared, count);
	return result;
}

static bool hmos_valid_transfer(const struct hmos_dmabuf_sync_file *request)
{
	return request->dma_buf_fd >= 0 && !request->reserved && request->flags &&
		!(request->flags & ~HMOS_DMABUF_SYNC_VALID_FLAGS);
}

static long hmos_export(void __user *user)
{
	struct hmos_dmabuf_sync_file request;
	struct dma_buf *buffer;
	struct dma_fence *fence;
	struct sync_file *sync;
	int fd, error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!hmos_valid_transfer(&request) || request.fence_fd != -1)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		error = fd;
		goto put_buffer;
	}
	fence = hmos_snapshot(buffer->resv, request.flags & HMOS_DMABUF_SYNC_WRITE);
	if (IS_ERR(fence)) {
		error = PTR_ERR(fence);
		goto put_fd;
	}
	sync = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync) {
		error = -ENOMEM;
		goto put_fd;
	}
	request.fence_fd = fd;
	// 先复制描述符再安装文件，复制失败时完整回滚
	if (copy_to_user(user, &request, sizeof(request))) {
		error = -EFAULT;
		fput(sync->file);
		goto put_fd;
	}
	hmos_publish_fence(sync->fence);
	fd_install(fd, sync->file);
	dma_buf_put(buffer);
	return 0;

put_fd:
	put_unused_fd(fd);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long hmos_import(void __user *user)
{
	struct hmos_dmabuf_sync_file request;
	struct dma_fence **previous = NULL;
	struct dma_fence *fence, *merged;
	struct dma_buf *buffer;
	unsigned int count = 0;
	int error;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (!hmos_valid_transfer(&request) || request.fence_fd < 0)
		return -EINVAL;
	buffer = dma_buf_get(request.dma_buf_fd);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fence = sync_file_get_fence(request.fence_fd);
	if (!fence) {
		error = -EINVAL;
		goto put_buffer;
	}
	error = dma_resv_lock_interruptible(buffer->resv, NULL);
	if (error)
		goto put_fence;
	if (request.flags & HMOS_DMABUF_SYNC_WRITE) {
		// 旧版独占槽会清空读者，先合并未完成依赖再替换
		error = hmos_dma_resv_get_fences(buffer->resv, NULL, &count, &previous);
		if (!error) {
			merged = hmos_merge_fences(previous, count, fence);
			if (IS_ERR(merged)) {
				error = PTR_ERR(merged);
			} else {
				hmos_publish_fence(merged);
				dma_resv_add_excl_fence(buffer->resv, merged);
				dma_fence_put(merged);
			}
		}
		hmos_put_fences(previous, count);
	} else {
		error = dma_resv_reserve_shared(buffer->resv, 1);
		if (!error)
			dma_resv_add_shared_fence(buffer->resv, fence);
	}
	dma_resv_unlock(buffer->resv);
put_fence:
	dma_fence_put(fence);
put_buffer:
	dma_buf_put(buffer);
	return error;
}

static long hmos_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct hmos_dmabuf_sync_abi query;
	void __user *user = (void __user *)argument;

	(void)file;
	switch (command) {
	case HMOS_DMABUF_SYNC_QUERY:
		if (copy_from_user(&query, user, sizeof(query)))
			return -EFAULT;
		if (query.version || query.features || query.reserved)
			return -EINVAL;
		query.version = HMOS_DMABUF_SYNC_VERSION;
		query.features = HMOS_DMABUF_SYNC_FEATURES_REQUIRED;
		query.reserved = 0;
		return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
	case HMOS_DMABUF_SYNC_EXPORT:
		return hmos_export(user);
	case HMOS_DMABUF_SYNC_IMPORT:
		return hmos_import(user);
	default:
		return -ENOTTY;
	}
}

#ifndef HMOS_SYNC_HOST_TEST
#ifdef CONFIG_COMPAT
static long hmos_compat_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	return hmos_ioctl(file, command, (unsigned long)compat_ptr(argument));
}
#endif

static const struct file_operations hmos_operations = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hmos_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = hmos_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice hmos_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hmos_dmabuf_sync",
	.fops = &hmos_operations,
	.mode = 0600,
};

static int __init hmos_init(void)
{
	int error;

	BUILD_BUG_ON(sizeof(struct hmos_dmabuf_sync_abi) != 16);
	BUILD_BUG_ON(sizeof(struct hmos_dmabuf_sync_file) != 16);
	error = hmos_context_initialize();
	return error ? error : misc_register(&hmos_device);
}

static void __exit hmos_exit(void)
{
	misc_deregister(&hmos_device);
}

module_init(hmos_init);
module_exit(hmos_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Owned dma-buf reservation and sync-file bridge");
#endif
