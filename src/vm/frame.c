#include "lib/kernel/list.h"
#include "vm/frame.h"

// each frame is a 32 bit integer, it's self contained with all information
// frame table is a linked list, for easy evicting
struct list frame_table;

void
frame_init ()
{
    list_init(&frame_table);
}

void *
frame_get_page (enum palloc_flags flags) 
{
    // Get a single frame from palloc.c, assume this is already a 32 bit address containing all meta data
    // Assume this already contains all meta data for evicting(pining, dirty, etc.)
    void * res = palloc_get_page(flags);
    //TODO: this could fail if all frames are full, evict

    // Add this new page to frame_table
    list_push_back(&frame_table, res);

    return res;
}

void
frame_free_page (void *page)
{
  // free this page

  //TODO: may need to overwrite all evicting related bits
  list_remove(&frame_table, page);
}

