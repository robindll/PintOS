void frame_init ();

void * frame_get_page (enum palloc_flags flags);

void frame_free_page (void *page);
