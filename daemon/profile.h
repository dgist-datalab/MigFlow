#ifndef MIGFLOW_PROFILE_H
#define MIGFLOW_PROFILE_H
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "migflow.h"

#define MAX_EVENTS 10
#define EPOLL_TIMEOUT 10

int init_profile(int pid);
void destroy_profile(void);
int setup_drain(void);
int start_profile(void);
int drain(bool &alloc_occured, bool &profile_occured);
int profile_pages(int age);
unsigned long do_cooling(struct hist_bin *hist, int cur_age);
void print_hist(struct hist_bin *hist, bool clear_stat);
void add_hist_bin_va(struct hist_bin *bin, unsigned long va, struct page_profile *page_info, int node);
void delete_hist_bin_va(struct hist_bin *bin, unsigned long va, int node);

#endif
