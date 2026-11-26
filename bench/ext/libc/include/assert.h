/* BPX-1 bench/ext: minimal <assert.h>. A failed assertion ends the program with exit code 97. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void bpx_ext_assert_fail(void) __attribute__((noreturn));
#define assert(e) ((e) ? (void)0 : bpx_ext_assert_fail())
#endif
