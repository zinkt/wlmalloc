#include "wlgc.h"
#include <stdlib.h>
#include <setjmp.h>
#include "log.h"

// 全局内存回收器
gc_t gc;

#undef LOGLEVEL
#define LOGLEVEL LOGLEVEL_DEBUG

void gc_init() {
    log_debug("gc init");
    // 将bos设置为调用函数的起始地址
    gc.bos = __builtin_frame_address(1);
    gc.size = 0;
    gc.sweep_limit = (int)(DEFAULT_CAPACITY * SWEEP_FACTOR);
    
}

static char gc_need_collect() {return gc.size > gc.sweep_limit;}

static alloc_t *search(void *p) {
    log_debug("search alloc: %p", p);
    // 在DEFAULT_CAPACITY为2的整次幂时，使用& (DEFAULT_CAPACITY-1)这种取模方式更快
    alloc_t *ret = gc.allocs[hash(p) & (DEFAULT_CAPACITY-1)];
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
    log_debug("mark alloc(%p) and it's children", alloc);
    // 如果已标记，则跳过
    if(alloc && !(alloc->size_tag & 0x1)) {
        // 标记
        alloc->size_tag & 0x1;
        // 遍历对象内存，尝试找到其子对象
        char *p1 = alloc->p;
        char *p2 = alloc->p + ((alloc->size_tag)&~0x1) - sizeof(void *);
        for(; p1 <= p2; p1++) {
            gc_mark_alloc(p1);
        }
    }
}

static void gc_mark() {
    log_debug("start mark phase");
    // 扫描栈，获取根节点，并直接从根节点开始递归标记
    jmp_buf jb;
    setjmp(jb);
    __READ_RSP();
    char *tos = __rsp;
    for(char *p = tos; p <= gc.bos; ++p) {
        alloc_t *alloc = search(p);
        if(alloc) {
            gc_mark_alloc(alloc);
        }
    }
}

static void gc_free_alloc(alloc_t *alloc) {
    if(!alloc) return;

    // 将其从哈希表中删去
    int index = hash(alloc->p) & (DEFAULT_CAPACITY-1);
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

    free(alloc->p);
    free(alloc);
    --gc.size;
}

void gc_sweep() {
    if(!gc.size) return;
    // 遍历哈希表
    for(int i = 0; i < DEFAULT_CAPACITY; i++) {
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
}

void gc_collect() {
    gc_mark();
    gc_sweep();
}

void *gc_malloc(size_t size) {
    if(!size) return NULL;
    if(gc_need_collect()) {
        gc_collect();
    }
    size_t sz = ROUNDUP(size, 8);
    void *ret = malloc(sz);
    if(ret) {
        alloc_t *alloc = malloc(sizeof(alloc_t));
        alloc->p = ret;
        alloc->size_tag = sz;
        alloc->next = NULL;
        alloc->destructor = NULL;
        // 插入到hash表中
        int index = hash(ret)&(DEFAULT_CAPACITY-1);
        alloc_t * a = gc.allocs[index];
        gc.allocs[index] = alloc;
        alloc->next = a;
    }
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