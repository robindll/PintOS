#include <bitmap.h>

#include "threads/vaddr.h"
#include "devices/block.h"
#include "vm/swap.h"

static struct block* swap_slots;                   // Swap slots
static struct bitmap* available_slot_bitmap;       // Bitmap recording available slots

static const size_t SECTORS_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE; // How many sectors is needed for storing one page content

// The number of possible swapped pages
static size_t max_swap_page_count;

/**
 * Initialize swap, Must be called ONLY ONCE at the initialization phase.
 */
void swap_init (void)
{
    ASSERT (SECTORS_PER_PAGE > 0);

    // Initialize the swap disk
    swap_slots = block_get_role(BLOCK_SWAP);
    if (swap_slots == NULL) {
        PANIC ("Error: Can't initialize swap block");
    }

    // Initialize available_slot_bitmap, with all entry true
    // each single bit of available_slot_bitmap corresponds to a block slot,
    // which consists of contiguous SECTORS_PER_PAGE sectors,
    // their total size being equal to PGSIZE.
    max_swap_page_count = block_size(swap_slots) / SECTORS_PER_PAGE;
    available_slot_bitmap = bitmap_create(max_swap_page_count);
    bitmap_set_all(available_slot_bitmap, true);
}


/**
 * Swap out the content in given page into swap disk.
 * Return the index of swap slot in which it is placed.
 */
uint32_t swap_out (void* page)
{
    // Ensure that the page is on user's virtual memory.
    ASSERT (page >= PHYS_BASE);

    // Find an available block slot to use
    size_t swap_index = bitmap_scan (available_slot_bitmap, /*start*/0, /*cnt*/1, true);

    // Write all content to swap slot
    size_t i = 0;
    for (; i < SECTORS_PER_PAGE; ++i) {
        block_write(swap_slots,
            /* sector number */  swap_index * SECTORS_PER_PAGE + i,
            /* target address */ (char*)page + (BLOCK_SECTOR_SIZE * i));
    }

    // Mark the slot is used
    bitmap_set(available_slot_bitmap, swap_index, false);

    return swap_index;
}


/**
 * Read content in in swap_index slot on swap back into given page
 */
void swap_in (uint32_t swap_index, void* page)
{
    // Ensure that the page is on user's virtual memory.
    ASSERT (page >= PHYS_BASE);

    // check the swap slot
    ASSERT (swap_index < max_swap_page_count);
    if (bitmap_test (available_slot_bitmap, swap_index) == true) {
        // Trying to swap an unassigned swap slot in, error
        PANIC ("Error, invalid read access to unassigned swap block");
    }

    // Read a page content back from swap slot
    size_t i = 0;
    for (; i < SECTORS_PER_PAGE; ++i) {
        block_read (swap_slots,
            /* sector number */  swap_index * SECTORS_PER_PAGE + i,
            /* target address */ (char*)page + (BLOCK_SECTOR_SIZE * i)
            );
    }

    // Mark swap slot available
    bitmap_set(available_slot_bitmap, swap_index, true);
}

/**
 * Drop swap slot
 */
void swap_free (uint32_t swap_index)
{
    // check the swap region
    ASSERT (swap_index < max_swap_page_count);

    if (bitmap_test (available_slot_bitmap, swap_index) == true) {
        PANIC ("Error, invalid free request to unassigned swap block");
    }

    // Mark swap slot is available
    bitmap_set(available_slot_bitmap, swap_index, true);
}
