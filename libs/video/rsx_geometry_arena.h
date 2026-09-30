/* Geometry cache: immutable slices of upload buffers. A slice retains the
 * existing per-buffer retirement fence and owns one resource reference.
 * Textually included by rsx_geometry_cache.h, which supplies g_geometry_cache,
 * LD_GEOMETRY_GPU_LIMIT and ld_geometry_buffer. */
typedef struct ld_geometry_arena {
    ID3D12Resource* resource;
    u8* mapped;
    u64 bytes, used;
    struct ld_geometry_arena* next;
} ld_geometry_arena;
static ld_geometry_arena* g_geometry_arenas;
static int ld_geometry_arena_allocate(u64 capacity,ld_geometry_buffer* buffer)
{
    if(!capacity || (capacity&255u) || capacity>LD_GEOMETRY_GPU_LIMIT)return 0;
    ld_geometry_arena* arena=g_geometry_arenas;
    while(arena && capacity>arena->bytes-arena->used)arena=arena->next;
    if(!arena) {
        const u64 remaining=LD_GEOMETRY_GPU_LIMIT-g_geometry_cache.gpu_bytes;
        u64 bytes=capacity>4u*1024u*1024u?capacity:4u*1024u*1024u;
        bytes=(bytes+65535u)&~65535ull;
        if(bytes>remaining)bytes=remaining&~65535ull;
        if(bytes<capacity)return 0;
        arena=calloc(1,sizeof(*arena));if(!arena)return 0;
        D3D12_HEAP_PROPERTIES heap={0};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc={0};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev,&heap,D3D12_HEAP_FLAG_NONE,
            &desc,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,
            (void**)&arena->resource))){free(arena);return 0;}
        D3D12_RANGE read={0,0};
        if(FAILED(arena->resource->lpVtbl->Map(arena->resource,0,&read,(void**)&arena->mapped))){
            arena->resource->lpVtbl->Release(arena->resource);free(arena);return 0;
        }
        arena->bytes=bytes;arena->next=g_geometry_arenas;g_geometry_arenas=arena;
        g_geometry_cache.gpu_bytes+=bytes;
    }
    buffer->resource=arena->resource;
    buffer->resource->lpVtbl->AddRef(buffer->resource);
    buffer->mapped=arena->mapped+arena->used;
    buffer->resource_offset=arena->used;
    buffer->capacity=capacity;
    arena->used+=capacity;
    return 1;
}
static void ld_geometry_arena_destroy(void)
{
    while(g_geometry_arenas) {
        ld_geometry_arena* next=g_geometry_arenas->next;
        g_geometry_arenas->resource->lpVtbl->Release(g_geometry_arenas->resource);
        free(g_geometry_arenas);g_geometry_arenas=next;
    }
}
