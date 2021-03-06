#ifndef VM_SWAP_HEADER
#define VM_SWAP_HEADER

#define NO_SAWP_INDEX -1

/**
 * Initialize swap, Must be called ONLY ONCE at the initialization phase.
 */
void swap_init (void);


/**
 * Swap out the content in given page into swap disk.
 * Return the index of swap slot in which it is placed.
 */
uint32_t swap_out (void* page);


/**
 * Read content in in swap_index slot on swap back into given page
 */
void swap_in (uint32_t swap_index, void* page);

/**
 * Drop swap slot
 */
void swap_free (uint32_t swap_index);

#endif