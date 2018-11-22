#include <bitmap.h>

#include "threads/vaddr.h"
#include "devices/block.h"
#include "vm/swap.h"

static struct block* swap_block;            // Swap block
static struct bitmap* swap_available;       // Bitmap recording available slots

static const size_t SECTORS_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;

// The number of possible swapped pages
static size_t swap_size;

/**
 * Initialize swap, Must be called ONLY ONCE at the initialization phase.
 */
void vm_swap_init (void)
{
    ASSERT (SECTORS_PER_PAGE > 0);

    // Initialize the swap disk
    swap_block = block_get_role(BLOCK_SWAP);
    if (swap_block == NULL) {
        PANIC ("Error: Can't initialize swap block");
    }

    // Initialize swap_available, with all entry true
    // each single bit of `swap_available` corresponds to a block slot,
    // which consists of contiguous SECTORS_PER_PAGE sectors,
    // their total size being equal to PGSIZE.
    swap_size = block_size(swap_block) / SECTORS_PER_PAGE;
    swap_available = bitmap_create(swap_size);
    bitmap_set_all(swap_available, true);
}


/**
 * Swap out the content in given page into swap disk.
 * Return the index of swap slot in which it is placed.
 */
uint32_t vm_swap_out (void* page)
{
    // Ensure that the page is on user's virtual memory.
    ASSERT (page >= PHYS_BASE);

    // Find an available block slot to use
    size_t swap_index = bitmap_scan (swap_available, /*start*/0, /*cnt*/1, true);

    // Write all content to swap slot
    size_t i = 0;
    for (; i < SECTORS_PER_PAGE; ++i) {
        block_write(swap_block,
            /* sector number */  swap_index * SECTORS_PER_PAGE + i,
            /* target address */ (char*)page + (BLOCK_SECTOR_SIZE * i));
    }

    // Mark the slot is used
    bitmap_set(swap_available, swap_index, false);

    return swap_index;
}


/**
 * Read content in in swap_index slot on swap back into given page
 */
void vm_swap_in (uint32_t swap_index, void* page)
{
    // Ensure that the page is on user's virtual memory.
    ASSERT (page >= PHYS_BASE);

    // check the swap slot
    ASSERT (swap_index < swap_size);
    if (bitmap_test (swap_available, swap_index) == true) {
        // Trying to swap an unassigned swap slot in, error
        PANIC ("Error, invalid read access to unassigned swap block");
    }

    // Read a page content back from swap slot
    size_t i = 0;
    for (; i < SECTORS_PER_PAGE; ++i) {
        block_read (swap_block,
            /* sector number */  swap_index * SECTORS_PER_PAGE + i,
            /* target address */ (char*)page + (BLOCK_SECTOR_SIZE * i)
            );
    }

    // Mark swap slot available
    bitmap_set(swap_available, swap_index, true);
}

/**
 * Drop swap slot
 */
void vm_swap_free (uint32_t swap_index)
{
    // check the swap region
    ASSERT (swap_index < swap_size);

    if (bitmap_test (swap_available, swap_index) == true) {
        PANIC ("Error, invalid free request to unassigned swap block");
    }

    // Mark swap slot is available
    bitmap_set(swap_available, swap_index, true);
}
