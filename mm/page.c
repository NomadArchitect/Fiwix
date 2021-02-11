/*
 * fiwix/mm/page.c
 *
 * Copyright 2018, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

/*
 * page.c implements a cache with a free list as a doubly circular linked
 * list and a chained hash table with doubly linked lists.
 *
 * hash table
 * +--------+  +--------------+  +--------------+  +--------------+
 * | index  |  |prev|data|next|  |prev|data|next|  |prev|data|next|
 * |   0   --> | /  |    | --->  <--- |    | --->  <--- |    |  / |
 * +--------+  +--------------+  +--------------+  +--------------+
 * +--------+  +--------------+  +--------------+  +--------------+
 * | index  |  |prev|data|next|  |prev|data|next|  |prev|data|next|
 * |   1   --> | /  |    | --->  <--- |    | --->  <--- |    |  / |
 * +--------+  +--------------+  +--------------+  +--------------+
 *              (page)            (page)            (page)  
 *    ...
 */

#include <fiwix/asm.h>
#include <fiwix/kernel.h>
#include <fiwix/mm.h>
#include <fiwix/mman.h>
#include <fiwix/bios.h>
#include <fiwix/sleep.h>
#include <fiwix/sched.h>
#include <fiwix/devices.h>
#include <fiwix/buffer.h>
#include <fiwix/errno.h>
#include <fiwix/stdio.h>
#include <fiwix/string.h>

#define PAGE_HASH(inode, offset)	(((__ino_t)(inode) ^ (__off_t)(offset)) % NR_PAGE_HASH)
#define NR_PAGES	page_table_size / sizeof(struct page)
#define NR_PAGE_HASH	page_hash_table_size / sizeof(unsigned int)

struct page *page_table;		/* page pool */
struct page *page_head;			/* page pool head */
struct page **page_hash_table;

static void insert_to_hash(struct page *pg)
{
	struct page **h;
	int i;

	i = PAGE_HASH(pg->inode->inode, pg->offset);
	h = &page_hash_table[i];

	if(!*h) {
		*h = pg;
		(*h)->prev_hash = (*h)->next_hash = NULL;
	} else {
		pg->prev_hash = NULL;
		pg->next_hash = *h;
		(*h)->prev_hash = pg;
		*h = pg;
	}
	kstat.cached += (PAGE_SIZE / 1024);
}

static void remove_from_hash(struct page *pg)
{
	struct page **h;
	int i;

	if(!pg->inode) {
		return;
	}

	i = PAGE_HASH(pg->inode->inode, pg->offset);
	h = &page_hash_table[i];

	while(*h) {
		if(*h == pg) {
			if((*h)->next_hash) {
				(*h)->next_hash->prev_hash = (*h)->prev_hash;
			}
			if((*h)->prev_hash) {
				(*h)->prev_hash->next_hash = (*h)->next_hash;
			}
			if(h == &page_hash_table[i]) {
				*h = (*h)->next_hash;
			}
			kstat.cached -= (PAGE_SIZE / 1024);
			break;
		}
		h = &(*h)->next_hash;
	}
}

static void remove_from_free_list(struct page *pg)
{
	pg->prev_free->next_free = pg->next_free;
	pg->next_free->prev_free = pg->prev_free;
	kstat.free_pages--;
	if(pg == page_head) {
		page_head = pg->next_free;
	}
}

void page_lock(struct page *pg)
{
	unsigned long int flags;

	for(;;) {
		SAVE_FLAGS(flags); CLI();
		if(pg->locked) {
			RESTORE_FLAGS(flags);
			sleep(&pg, PROC_UNINTERRUPTIBLE);
		} else {
			break;
		}
	}
	pg->locked = 1;
	RESTORE_FLAGS(flags);
}

void page_unlock(struct page *pg)
{
	unsigned long int flags;

	SAVE_FLAGS(flags); CLI();
	pg->locked = 0;
	wakeup(pg);
	RESTORE_FLAGS(flags);
}

struct page * get_free_page(void)
{
	unsigned long int flags;
	struct page *pg;

	/* if no more pages on free list */
	while(page_head == page_head->next_free) {
		/* reclaim some memory from buffer cache */
		wakeup(&kswapd);
		sleep(&get_free_page, PROC_UNINTERRUPTIBLE);

		if(page_head == page_head->next_free) {
			/* definitely out of memory! (no more pages) */
			printk("%s(): pid %d ran out of memory. OOM killer needed!\n", __FUNCTION__, current->pid);
			return NULL;
		}
	}

	SAVE_FLAGS(flags); CLI();

	pg = page_head;
	remove_from_free_list(pg);
	remove_from_hash(pg);	/* remove it from its old hash */
	pg->count = 1;
	pg->inode = NULL;
	pg->offset = 0;

	RESTORE_FLAGS(flags);
	return pg;
}

struct page * search_page_hash(struct inode *inode, __off_t offset)
{
	struct page *pg;
	int i;

	i = PAGE_HASH(inode->inode, offset);
	pg = page_hash_table[i];

	while(pg) {
		if(pg->inode == inode && pg->offset == offset) {
			if(!pg->count) {
				remove_from_free_list(pg);
			}
			pg->count++;
			return pg;
		}
		pg = pg->next_hash;
	}

	return NULL;
}

void release_page(unsigned int page)
{
	unsigned long int flags;
	struct page *pg;

	if(!valid_page(page)) {
		PANIC("Unexpected inconsistency in hash_table. Missing page %d (0x%x).\n", page, page);
	}

	pg = &page_table[page];
	if(--pg->count > 0) {
		return;
	}

	SAVE_FLAGS(flags); CLI();

	if(!page_head) {
		pg->prev_free = pg->next_free = pg;
		page_head = pg;
	} else {
		pg->next_free = page_head;
		pg->prev_free = page_head->prev_free;
		page_head->prev_free->next_free = pg;
		page_head->prev_free = pg;
	}

	/* if page is not cached then place it at the head of the free list */
	if(!pg->inode) {
		page_head = pg;
	}

	kstat.free_pages++;

	RESTORE_FLAGS(flags);

	/*
	 * We need to wait for free pages to be greater than NR_BUF_RECLAIM,
	 * otherwise get_free_pages() could run out of pages _again_, and it
	 * would think that 'definitely there are no more free pages', killing
	 * the current process prematurely.
	 */
	if(kstat.free_pages > NR_BUF_RECLAIM) {
		wakeup(&get_free_page);
	}
}

int valid_page(unsigned int page)
{
	return (page >= 0 && page < NR_PAGES);
}

void update_page_cache(struct inode *i, __off_t offset, const char *buf, int count)
{
	__off_t poffset;
	struct page *pg;
	int bytes;

	poffset = offset % PAGE_SIZE;
	offset &= PAGE_MASK;
	bytes = PAGE_SIZE - poffset;

	if(count) {
		bytes = MIN(bytes, count);
		if((pg = search_page_hash(i, offset))) {
			page_lock(pg);
			memcpy_b(pg->data + poffset, buf, bytes);
			page_unlock(pg);
			release_page(pg->page);
		}
	}
}

int write_page(struct page *pg, struct inode *i, __off_t offset, unsigned int length)
{
	struct fd fd_table;
	unsigned int size;
	int errno;

	size = MIN(i->i_size, length);
	fd_table.inode = i;
	fd_table.flags = 0;
	fd_table.count = 0;
	fd_table.offset = offset;
	if(i->fsop && i->fsop->write) {
		errno = i->fsop->write(i, &fd_table, pg->data, size);
	} else {
		errno = -EINVAL;
	}

	return errno;
}

int bread_page(struct page *pg, struct inode *i, __off_t offset, char prot, char flags)
{
	__blk_t block;
	__off_t size_read;
	int blksize;
	struct device *d;
	struct buffer *buf;

	blksize = i->sb->s_blocksize;
	size_read = 0;

	if(!(d = get_device(BLK_DEV, i->dev))) {
		printk("WARNING: %s: device major %d not found!\n", __FUNCTION__, MAJOR(i->dev));
		return 1;
	}
	if(!d->fsop || !d->fsop->read_block) {
		printk("WARNING: %s: device %d,%d does not have the read_block() method!\n", __FUNCTION__, MAJOR(i->dev), MINOR(i->dev));
		return 1;
	}

	pg->inode = i;
	pg->offset = offset;
	if(!(prot & PROT_WRITE) || flags & MAP_SHARED) {
		while(size_read < PAGE_SIZE) {
			if((block = bmap(i, offset, FOR_READING)) < 0) {
				return 1;
			}
			if(block) {
				/* does exist a buffer with recent data? */
				if(!(buf = get_dirty_buffer(i->dev, block, blksize))) {
					if(d->fsop->read_block(i->dev, block, pg->data + size_read, blksize) < 0) {
						return 1;
					}
				} else {
					memcpy_b(pg->data + size_read, buf->data, blksize);
					brelse(buf);
				}
			} else {
				/* fill the hole with zeros */
				memset_b(pg->data + size_read, 0, blksize);
			}
			size_read += blksize;
			offset += blksize;
		}
		/* cache all read-only and public (shared) pages */
		insert_to_hash(pg);
	} else {
		while(size_read < PAGE_SIZE) {
			if((block = bmap(i, offset, FOR_READING)) < 0) {
				return 1;
			}
			if(block) {
				/*
				 * This feeds the buffer cache by reading only
				 * the writable pages which aren't included in
				 * the page cache. This will speed up things by
				 * keeping in buffer cache the writable pages
				 * with its original (disk) content (i.e. pages
				 * from the data section of an ELF).
				 */
				if(!(buf = bread(i->dev, block, blksize))) {
					return 1;
				}
				memcpy_b(pg->data + size_read, buf->data, blksize);
				brelse(buf);
			}
			size_read += blksize;
			offset += blksize;
		}
		pg->inode = NULL;
		pg->offset = 0;
	}
	return 0;
}

int file_read(struct inode *i, struct fd *fd_table, char *buffer, __size_t count)
{
	__off_t total_read;
	unsigned int page, poffset, bytes;
	struct page *pg;

	inode_lock(i);

	if(fd_table->offset > i->i_size) {
		fd_table->offset = i->i_size;
	}

	total_read = 0;

	for(;;) {
		count = (fd_table->offset + count > i->i_size) ? i->i_size - fd_table->offset : count;
		if(!count) {
			break;
		}

		poffset = fd_table->offset % PAGE_SIZE;
		if(!(pg = search_page_hash(i, fd_table->offset & PAGE_MASK))) {
			if(!(page = kmalloc())) {
				inode_unlock(i);
				printk("%s(): returning -ENOMEM\n", __FUNCTION__);
				return -ENOMEM;
			}
			page = V2P(page);
			pg = &page_table[page >> PAGE_SHIFT];
			if(bread_page(pg, i, fd_table->offset & PAGE_MASK, 0, MAP_SHARED)) {
				kfree((unsigned int)pg->data);
				inode_unlock(i);
				printk("%s(): returning -EIO\n", __FUNCTION__);
				return -EIO;
			}
		}

		page_lock(pg);
		bytes = PAGE_SIZE - poffset;
		bytes = MIN(bytes, count);
		memcpy_b(buffer + total_read, pg->data + poffset, bytes);
		total_read += bytes;
		count -= bytes;
		poffset += bytes;
		poffset %= PAGE_SIZE;
		fd_table->offset += bytes;
		page_unlock(pg);
		kfree((unsigned int)pg->data);
	}

	inode_unlock(i);
	return total_read;
}

void page_init(unsigned int pages)
{
	struct page *pg;
	unsigned int n, addr;

	memset_b(page_table, NULL, page_table_size);
	memset_b(page_hash_table, NULL, page_hash_table_size);

	for(n = 0; n < pages; n++) {
		pg = &page_table[n];
		pg->page = n;

		addr = n << PAGE_SHIFT;
		if(addr >= KERNEL_ENTRY_ADDR && addr < V2P(_last_data_addr)) {
			pg->flags = PAGE_RESERVED;
			kstat.kernel_reserved++;
			continue;
		}

		/*
		 * Some memory addresses are reserved, like the memory between
		 * 0xA0000 and 0xFFFFF and other addresses, mostly used by the
		 * VGA graphics adapter and BIOS.
		 */
		if(!addr_in_bios_map(addr)) {
			pg->flags = PAGE_RESERVED;
			kstat.physical_reserved++;
			continue;
		}

		pg->data = (char *)P2V(addr);
		if(!page_head) {
			pg->prev_free = pg->next_free = pg;
			page_head = pg;
		} else {
			pg->next_free = page_head;
			pg->prev_free = page_head->prev_free;
			page_head->prev_free->next_free = pg;
			page_head->prev_free = pg;
		}
		kstat.free_pages++;
	}
	kstat.total_mem_pages = kstat.free_pages;
	kstat.kernel_reserved <<= 2;
	kstat.physical_reserved <<= 2;
}
