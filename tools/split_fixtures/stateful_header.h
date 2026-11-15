/* State at file scope in a header, which a split build must hold once:
   every unit including it would otherwise count on its own. */
static int64_t sh_count;
static inline void sh_bump(int64_t by) { sh_count += by; }
static int64_t sh_get(void) { return sh_count; }
