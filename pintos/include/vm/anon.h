#ifndef VM_ANON_H
#define VM_ANON_H
#include "devices/disk.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct anon_page {
  struct swap_anon *swap_anon;
};

struct swap_anon {
  bool use;  // 스왑 공간을 사용하고 있는지 여부
  disk_sector_t sector[8];
  struct page *page;
  struct list_elem swap_elem;
};

void vm_anon_init(void);
bool anon_initializer(struct page *page, enum vm_type type, void *kva);

#endif
