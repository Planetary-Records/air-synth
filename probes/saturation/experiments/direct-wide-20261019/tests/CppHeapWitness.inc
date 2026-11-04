namespace allocationWitness
{
thread_local bool enabled=false;
thread_local std::size_t allocations=0,frees=0;
void allocated() noexcept { if(enabled) ++allocations; }
void freed(void* pointer) noexcept { if(enabled&&pointer) ++frees; }
}
void* operator new(std::size_t size) { if(auto* p=std::malloc(size?size:1)){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { allocationWitness::freed(p);std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
#if defined(_WIN32)
void* operator new(std::size_t size,std::align_val_t alignment) { if(auto* p=_aligned_malloc(size?size:1,static_cast<std::size_t>(alignment))){allocationWitness::allocated();return p;}throw std::bad_alloc(); }
void* operator new[](std::size_t size,std::align_val_t alignment) { return ::operator new(size,alignment); }
void operator delete(void* p,std::align_val_t) noexcept { allocationWitness::freed(p);_aligned_free(p); }
void operator delete[](void* p,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete(void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
void operator delete[](void* p,std::size_t,std::align_val_t alignment) noexcept { ::operator delete(p,alignment); }
#endif

