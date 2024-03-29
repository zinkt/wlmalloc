#include "wlgc.h"
#include <stdlib.h>
#include <setjmp.h>
#include "log.h"

// 全局内存回收器
gc_t gc;

#undef LOGLEVEL
#define LOGLEVEL LOG_INFO

void gc_init() {
    // log_debug("gc init");
    // 将bos设置为调用函数的起始地址
    gc.bos = __builtin_frame_address(1);
    gc.capacity = DEFAULT_CAPACITY;
    gc.allocs = calloc(gc.capacity, sizeof(alloc_t *));
    gc.size = 0;
    gc.sweep_limit = (size_t)(gc.capacity * SWEEP_FACTOR);
    gc.gc_cnt = 0;
}

static char gc_need_collect() {return gc.size > gc.sweep_limit;}
static char gc_need_resize() {return gc.size > gc.capacity * LOAD_FACTOR;}
static alloc_t *search(void *p) {
    // log_debug("Search alloc: %p", p);
    // 在DEFAULT_CAPACITY为2的整次幂时，使用& (DEFAULT_CAPACITY-1)这种取模方式更快
    uint32_t index = hash_func((uint64_t)p) % gc.capacity;
    alloc_t *ret = gc.allocs[index];
    while (ret)
    {
        if(ret->p == p) {
            return ret;
        }
        ret = ret->next;
    }
    return NULL;
}


static void gc_mark_alloc(alloc_t *alloc) {
    // log_debug("Mark alloc(%p) and it's children", alloc);
    // 如果已标记，则跳过
    if(alloc && !(alloc->size_tag & 0x1)) {
        // 标记
        alloc->size_tag |= 0x1;
        // 遍历对象内存，尝试找到其子对象
        char *p1 = alloc->p;
        char *p2 = alloc->p + ((alloc->size_tag)&~0x1) - sizeof(void *);
        for(; p1 <= p2; p1++) {
            alloc_t *alloc = search((void *)*(uintptr_t *)p1);
            if(alloc) {
                gc_mark_alloc(alloc);
            }
        }
    }
}

static void gc_mark() {
    // log_debug("Start mark phase");
    // 扫描栈，获取根节点，并直接从根节点开始递归标记
    jmp_buf jb;
    setjmp(jb);
    __READ_RSP();
    char *tos = __rsp;
    for(char *p = tos; p <= (char *)gc.bos; ++p) {
        alloc_t *alloc = search((void *)*(uintptr_t *)p);
        if(alloc) {
            gc_mark_alloc(alloc);
        }
    }
}

static void gc_free_alloc(alloc_t *alloc) {
    // log_debug("Free an alloc");
    if(!alloc) return;

    // 将其从哈希表中删去
    uint32_t index = hash_func((uint64_t)(alloc->p)) % gc.capacity;
    alloc_t *p = gc.allocs[index], *q = NULL;
    // 如果是第一个
    if(p == alloc) {
        gc.allocs[index] = alloc->next;
    }else {
        q = p->next;
        while(q) {
            if(q == alloc) {
                p->next = q->next;
                break;
            }
            p = q;
            q = q->next;
        }
    }
    if(alloc != p && alloc != q) {
        log_fatal("free a nonexistent alloc");
    }
    free(alloc->p);
    free(alloc);
    --gc.size;
    ++gc.gc_cnt;
}

// 扩容hash表
static void gc_hash_resize() {
    log_debug("Resizing hash: double capacity");
    size_t new_cap = gc.capacity * 2;
    alloc_t **new_allocs = calloc(new_cap, sizeof(alloc_t *));
    for(size_t i = 0; i < gc.capacity; ++i) {
        alloc_t *alloc = gc.allocs[i];
        while(alloc) {
            alloc_t *next_alloc = alloc->next;
            uint32_t index = hash_func((uint64_t)(alloc->p)) & (new_cap-1);
            alloc->next = new_allocs[index];
            new_allocs[index] = alloc;
            alloc = next_alloc;
        }
    }
    free(gc.allocs);
    gc.capacity = new_cap;
    gc.allocs = new_allocs;
    gc.sweep_limit = gc.capacity * SWEEP_FACTOR;
}

static void gc_sweep() {
    if(!gc.size) return;
    // 遍历哈希表
    for(size_t i = 0; i < gc.capacity; i++) {
        alloc_t *alloc = gc.allocs[i];
        alloc_t *next = NULL;
        while(alloc) {
            // 正在使用的内存
            if(alloc->size_tag & 0x1) {
                alloc->size_tag &= ~0x1;
                alloc = alloc -> next;
            }else { // 清理垃圾内存
                next = alloc -> next;
                gc_free_alloc(alloc);
                alloc = next;
            }
        }
    }
    // 如果回收后仍超过SWEEP_FACTOR
    // 则触发hash_resize
    if(gc_need_collect() || gc_need_resize()) {
        gc_hash_resize();
    }
}

void gc_collect() {
    gc_mark();
    gc_sweep();
    // log_debug("End collecting");
}

static void register_alloc(void *p, size_t sz) {
    if(p) {
        alloc_t *alloc = malloc(sizeof(alloc_t));
        alloc->p = p;
        alloc->size_tag = sz;
        alloc->next = NULL;
        // 插入到hash表中
        uint32_t index = hash_func((uint64_t)p) % gc.capacity;
        alloc_t *a = gc.allocs[index];
        gc.allocs[index] = alloc;
        alloc->next = a;
        ++gc.size;
    }
}

void *gc_malloc(size_t size) {
    if(!size) return NULL;
    if(gc_need_collect()) {
        gc_collect();
    }
    size_t sz = ROUNDUP(size, 8);
    void *ret = malloc(sz);
    register_alloc(ret, sz);
    return ret;
}

void *gc_calloc(size_t nmemb, size_t size) {
    if(!nmemb || !size) return NULL;
    if(gc_need_collect()) {
        gc_collect();
    }
    size_t sz = ROUNDUP(size, 8);
    void *ret = calloc(nmemb, sz);
    register_alloc(ret, sz * nmemb);
    return ret;
}

void *gc_realloc(void *ptr, size_t size) {
    if(!size) return NULL;
    if(gc_need_collect()) {
        gc_collect();
    }
    size_t sz = ROUNDUP(size, 8);
    void *ret = realloc(ptr, sz);
    register_alloc(ret, sz);
    return ret;
}

void gc_free(void *ptr) {
    if(!ptr) return;
    alloc_t *alloc = search(ptr);
    if(alloc) {
        gc_free_alloc(alloc);
    }else {
        log_warn("Ignoring request to free unknown pointer %p", ptr);
        free(ptr);
    }
}

size_t gc_collected() {return gc.gc_cnt;}