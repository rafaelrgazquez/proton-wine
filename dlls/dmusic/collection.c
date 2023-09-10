/*
 * IDirectMusicCollection Implementation
 *
 * Copyright (C) 2003-2004 Rok Mandeljc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dmusic_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmusic);
WINE_DECLARE_DEBUG_CHANNEL(dmfile);

struct instrument_entry
{
    struct list entry;
    IDirectMusicInstrument *instrument;
};

struct pool
{
    POOLTABLE table;
    POOLCUE cues[];
};

C_ASSERT(sizeof(struct pool) == offsetof(struct pool, cues[0]));

struct collection
{
    IDirectMusicCollection IDirectMusicCollection_iface;
    struct dmobject dmobj;
    LONG ref;

    IStream *pStm; /* stream from which we load collection and later instruments */
    DLSHEADER *pHeader;

    struct pool *pool;
    struct list instruments;
};

static inline struct collection *impl_from_IDirectMusicCollection(IDirectMusicCollection *iface)
{
    return CONTAINING_RECORD(iface, struct collection, IDirectMusicCollection_iface);
}

static inline struct collection *impl_from_IPersistStream(IPersistStream *iface)
{
    return CONTAINING_RECORD(iface, struct collection, dmobj.IPersistStream_iface);
}

static HRESULT WINAPI collection_QueryInterface(IDirectMusicCollection *iface,
        REFIID riid, void **ret_iface)
{
    struct collection *This = impl_from_IDirectMusicCollection(iface);

    TRACE("(%p, %s, %p)\n", iface, debugstr_dmguid(riid), ret_iface);

    *ret_iface = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicCollection))
        *ret_iface = iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject))
        *ret_iface = &This->dmobj.IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream))
        *ret_iface = &This->dmobj.IPersistStream_iface;
    else
    {
        WARN("(%p, %s, %p): not found\n", iface, debugstr_dmguid(riid), ret_iface);
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ret_iface);
    return S_OK;
}

static ULONG WINAPI collection_AddRef(IDirectMusicCollection *iface)
{
    struct collection *This = impl_from_IDirectMusicCollection(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p): new ref = %lu\n", iface, ref);

    return ref;
}

static ULONG WINAPI collection_Release(IDirectMusicCollection *iface)
{
    struct collection *This = impl_from_IDirectMusicCollection(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p): new ref = %lu\n", iface, ref);

    if (!ref)
    {
        struct instrument_entry *instrument_entry;
        void *next;

        LIST_FOR_EACH_ENTRY_SAFE(instrument_entry, next, &This->instruments, struct instrument_entry, entry)
        {
            list_remove(&instrument_entry->entry);
            IDirectMusicInstrument_Release(instrument_entry->instrument);
            free(instrument_entry);
        }

        free(This);
    }

    return ref;
}

static HRESULT WINAPI collection_GetInstrument(IDirectMusicCollection *iface,
        DWORD patch, IDirectMusicInstrument **instrument)
{
    struct collection *This = impl_from_IDirectMusicCollection(iface);
    struct instrument_entry *entry;
    DWORD inst_patch;
    HRESULT hr;

    TRACE("(%p, %lu, %p)\n", iface, patch, instrument);

    LIST_FOR_EACH_ENTRY(entry, &This->instruments, struct instrument_entry, entry)
    {
        if (FAILED(hr = IDirectMusicInstrument_GetPatch(entry->instrument, &inst_patch))) return hr;
        if (patch == inst_patch)
        {
            *instrument = entry->instrument;
            IDirectMusicInstrument_AddRef(entry->instrument);
            TRACE(": returning instrument %p\n", entry->instrument);
            return S_OK;
        }
    }

    TRACE(": instrument not found\n");
    return DMUS_E_INVALIDPATCH;
}

static HRESULT WINAPI collection_EnumInstrument(IDirectMusicCollection *iface,
        DWORD index, DWORD *patch, LPWSTR name, DWORD name_length)
{
    struct collection *This = impl_from_IDirectMusicCollection(iface);
    struct instrument_entry *entry;
    HRESULT hr;

    TRACE("(%p, %ld, %p, %p, %ld)\n", iface, index, patch, name, name_length);

    LIST_FOR_EACH_ENTRY(entry, &This->instruments, struct instrument_entry, entry)
    {
        if (index--) continue;
        if (FAILED(hr = IDirectMusicInstrument_GetPatch(entry->instrument, patch))) return hr;
        if (name)
        {
            struct instrument *instrument = impl_from_IDirectMusicInstrument(entry->instrument);
            DWORD length = min(lstrlenW(instrument->wszName), name_length - 1);
            memcpy(name, instrument->wszName, length * sizeof(WCHAR));
            name[length] = '\0';
        }
        return S_OK;
    }

    return S_FALSE;
}

static const IDirectMusicCollectionVtbl collection_vtbl =
{
    collection_QueryInterface,
    collection_AddRef,
    collection_Release,
    collection_GetInstrument,
    collection_EnumInstrument,
};

static HRESULT parse_lins_list(struct collection *This, IStream *stream, struct chunk_entry *parent)
{
    struct chunk_entry chunk = {.parent = parent};
    struct instrument_entry *entry;
    HRESULT hr;

    while ((hr = stream_next_chunk(stream, &chunk)) == S_OK)
    {
        switch (MAKE_IDTYPE(chunk.id, chunk.type))
        {
        case MAKE_IDTYPE(FOURCC_LIST, FOURCC_INS):
            if (!(entry = malloc(sizeof(*entry)))) return E_OUTOFMEMORY;
            hr = instrument_create_from_chunk(stream, &chunk, &entry->instrument);
            if (SUCCEEDED(hr)) list_add_tail(&This->instruments, &entry->entry);
            else free(entry);
            break;

        default:
            FIXME("Ignoring chunk %s %s\n", debugstr_fourcc(chunk.id), debugstr_fourcc(chunk.type));
            break;
        }

        if (FAILED(hr)) break;
    }

    return hr;
}

static HRESULT parse_ptbl_chunk(struct collection *This, IStream *stream, struct chunk_entry *chunk)
{
    struct pool *pool;
    POOLTABLE table;
    HRESULT hr;
    UINT size;

    if (chunk->size < sizeof(table)) return E_INVALIDARG;
    if (FAILED(hr = stream_read(stream, &table, sizeof(table)))) return hr;
    if (chunk->size != table.cbSize + sizeof(POOLCUE) * table.cCues) return E_INVALIDARG;
    if (table.cbSize != sizeof(table)) return E_INVALIDARG;

    size = offsetof(struct pool, cues[table.cCues]);
    if (!(pool = malloc(size))) return E_OUTOFMEMORY;
    pool->table = table;

    size = sizeof(POOLCUE) * table.cCues;
    if (FAILED(hr = stream_read(stream, pool->cues, size))) free(pool);
    else This->pool = pool;

    return hr;
}

static HRESULT WINAPI collection_object_ParseDescriptor(IDirectMusicObject *iface,
        IStream *stream, DMUS_OBJECTDESC *desc)
{
    struct chunk_entry riff = {0};
    HRESULT hr;

    TRACE("(%p, %p, %p)\n", iface, stream, desc);

    if (!stream || !desc)
        return E_POINTER;

    if ((hr = stream_get_chunk(stream, &riff)) != S_OK)
        return hr;
    if (riff.id != FOURCC_RIFF || riff.type != FOURCC_DLS) {
        TRACE("loading failed: unexpected %s\n", debugstr_chunk(&riff));
        stream_skip_chunk(stream, &riff);
        return DMUS_E_NOTADLSCOL;
    }

    hr = dmobj_parsedescriptor(stream, &riff, desc, DMUS_OBJ_NAME_INFO|DMUS_OBJ_VERSION);
    if (FAILED(hr))
        return hr;

    desc->guidClass = CLSID_DirectMusicCollection;
    desc->dwValidData |= DMUS_OBJ_CLASS;

    TRACE("returning descriptor:\n");
    dump_DMUS_OBJECTDESC(desc);
    return S_OK;
}

static const IDirectMusicObjectVtbl collection_object_vtbl =
{
    dmobj_IDirectMusicObject_QueryInterface,
    dmobj_IDirectMusicObject_AddRef,
    dmobj_IDirectMusicObject_Release,
    dmobj_IDirectMusicObject_GetDescriptor,
    dmobj_IDirectMusicObject_SetDescriptor,
    collection_object_ParseDescriptor,
};

static HRESULT WINAPI collection_stream_Load(IPersistStream *iface,
        IStream *stream)
{
    struct chunk_entry dls_chunk = {0};
    struct collection *This = impl_from_IPersistStream(iface);
    DMUS_PRIVATE_CHUNK chunk;
    DWORD StreamSize, StreamCount;
    LARGE_INTEGER liMove; /* used when skipping chunks */

    IStream_AddRef(stream); /* add count for later references */
    This->pStm = stream;

    IStream_Read(stream, &chunk, sizeof(FOURCC) + sizeof(DWORD), NULL);
    TRACE_(dmfile)(": %s chunk (size = %#04lx)", debugstr_fourcc(chunk.fccID), chunk.dwSize);

    if (chunk.fccID != FOURCC_RIFF) {
        TRACE_(dmfile)(": unexpected chunk; loading failed)\n");
        liMove.QuadPart = chunk.dwSize;
        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL); /* skip the rest of the chunk */
        return E_FAIL;
    }

    IStream_Read(stream, &chunk.fccID, sizeof(FOURCC), NULL);
    TRACE_(dmfile)(": RIFF chunk of type %s", debugstr_fourcc(chunk.fccID));
    StreamSize = chunk.dwSize - sizeof(FOURCC);
    StreamCount = 0;

    if (chunk.fccID != FOURCC_DLS) {
        TRACE_(dmfile)(": unexpected chunk; loading failed)\n");
        liMove.QuadPart = StreamSize;
        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL); /* skip the rest of the chunk */
        return E_FAIL;
    }

    dls_chunk.id = FOURCC_RIFF;
    dls_chunk.size = chunk.dwSize;
    dls_chunk.type = chunk.fccID;
    liMove.QuadPart = 0;
    IStream_Seek(stream, liMove, STREAM_SEEK_CUR, &dls_chunk.offset);
    dls_chunk.offset.QuadPart -= 12;
    if (FAILED(dmobj_parsedescriptor(stream, &dls_chunk, &This->dmobj.desc,
            DMUS_OBJ_NAME_INFO|DMUS_OBJ_VERSION|DMUS_OBJ_OBJECT|DMUS_OBJ_GUID_DLID)))
    {
        liMove.QuadPart = StreamSize;
        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL); /* skip the rest of the chunk */
        return E_FAIL;
    }
    stream_reset_chunk_data(stream, &dls_chunk);

    TRACE_(dmfile)(": collection form\n");
    do {
        IStream_Read(stream, &chunk, sizeof(FOURCC) + sizeof(DWORD), NULL);
        StreamCount += sizeof(FOURCC) + sizeof(DWORD) + chunk.dwSize;
        TRACE_(dmfile)(": %s chunk (size = %#04lx)", debugstr_fourcc(chunk.fccID), chunk.dwSize);
        switch (chunk.fccID) {
            case FOURCC_COLH: {
                TRACE_(dmfile)(": collection header chunk\n");
                This->pHeader = calloc(1, chunk.dwSize);
                IStream_Read(stream, This->pHeader, chunk.dwSize, NULL);
                break;
            }
            case FOURCC_PTBL: {
                struct chunk_entry ptbl_chunk = {.id = FOURCC_LIST, .size = chunk.dwSize, .type = chunk.fccID};
                TRACE_(dmfile)(": pool table chunk\n");
                liMove.QuadPart = 0;
                IStream_Seek(stream, liMove, STREAM_SEEK_CUR, &ptbl_chunk.offset);
                ptbl_chunk.offset.QuadPart -= 12;
                parse_ptbl_chunk(This, stream, &ptbl_chunk);
                stream_skip_chunk(stream, &ptbl_chunk);
                break;
            }
            case FOURCC_LIST: {
                IStream_Read(stream, &chunk.fccID, sizeof(FOURCC), NULL);
                TRACE_(dmfile)(": LIST chunk of type %s", debugstr_fourcc(chunk.fccID));
                switch (chunk.fccID) {
                    case FOURCC_WVPL: {
                        TRACE_(dmfile)(": wave pool list (mark & skip)\n");
                        liMove.QuadPart = chunk.dwSize - sizeof(FOURCC);
                        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL);
                        break;
                    }
                    case FOURCC_LINS: {
                        struct chunk_entry lins_chunk = {.id = FOURCC_LIST, .size = chunk.dwSize, .type = chunk.fccID};
                        TRACE_(dmfile)(": instruments list\n");
                        liMove.QuadPart = 0;
                        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, &lins_chunk.offset);
                        lins_chunk.offset.QuadPart -= 12;
                        parse_lins_list(This, stream, &lins_chunk);
                        stream_skip_chunk(stream, &lins_chunk);
                        break;
                    }
                    default: {
                        TRACE_(dmfile)(": unknown (skipping)\n");
                        liMove.QuadPart = chunk.dwSize - sizeof(FOURCC);
                        IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL);
                        break;
                    }
                }
                break;
            }
            default: {
                TRACE_(dmfile)(": unknown chunk (irrelevant & skipping)\n");
                liMove.QuadPart = chunk.dwSize;
                IStream_Seek(stream, liMove, STREAM_SEEK_CUR, NULL);
                break;
            }
        }
        TRACE_(dmfile)(": StreamCount = %ld < StreamSize = %ld\n", StreamCount, StreamSize);
    } while (StreamCount < StreamSize);

    TRACE_(dmfile)(": reading finished\n");


    /* DEBUG: dumps whole collection object tree: */
    if (TRACE_ON(dmusic))
    {
        struct instrument_entry *entry;
        int i = 0;

        TRACE("*** IDirectMusicCollection (%p) ***\n", &This->IDirectMusicCollection_iface);
        dump_DMUS_OBJECTDESC(&This->dmobj.desc);

        TRACE(" - Collection header:\n");
        TRACE("    - cInstruments: %ld\n", This->pHeader->cInstruments);
        TRACE(" - Instruments:\n");

        LIST_FOR_EACH_ENTRY(entry, &This->instruments, struct instrument_entry, entry)
            TRACE("    - Instrument[%i]: %p\n", i++, entry->instrument);
    }

    return S_OK;
}

static const IPersistStreamVtbl collection_stream_vtbl =
{
    dmobj_IPersistStream_QueryInterface,
    dmobj_IPersistStream_AddRef,
    dmobj_IPersistStream_Release,
    unimpl_IPersistStream_GetClassID,
    unimpl_IPersistStream_IsDirty,
    collection_stream_Load,
    unimpl_IPersistStream_Save,
    unimpl_IPersistStream_GetSizeMax,
};

HRESULT collection_create(IUnknown **ret_iface)
{
    struct collection *collection;

    *ret_iface = NULL;
    if (!(collection = calloc(1, sizeof(*collection)))) return E_OUTOFMEMORY;
    collection->IDirectMusicCollection_iface.lpVtbl = &collection_vtbl;
    collection->ref = 1;
    dmobject_init(&collection->dmobj, &CLSID_DirectMusicCollection,
            (IUnknown *)&collection->IDirectMusicCollection_iface);
    collection->dmobj.IDirectMusicObject_iface.lpVtbl = &collection_object_vtbl;
    collection->dmobj.IPersistStream_iface.lpVtbl = &collection_stream_vtbl;
    list_init(&collection->instruments);

    TRACE("Created DirectMusicCollection %p\n", collection);
    *ret_iface = (IUnknown *)&collection->IDirectMusicCollection_iface;
    return S_OK;
}
