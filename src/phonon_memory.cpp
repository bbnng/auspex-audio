#include "phonon_memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <mutex>

namespace sa {
namespace {

using HeapAllocFn   = LPVOID (WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID (WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);

std::mutex    g_mutex;
int           g_users = 0;
HeapAllocFn   g_heap_alloc = nullptr;    // what phonon.dll called before
HeapReAllocFn g_heap_realloc = nullptr;
void**        g_alloc_slot = nullptr;
void**        g_realloc_slot = nullptr;

LPVOID WINAPI zeroed_alloc(HANDLE heap, DWORD flags, SIZE_T size) {
    return g_heap_alloc(heap, flags | HEAP_ZERO_MEMORY, size);
}

LPVOID WINAPI zeroed_realloc(HANDLE heap, DWORD flags, LPVOID memory, SIZE_T size) {
    return g_heap_realloc(heap, flags | HEAP_ZERO_MEMORY, memory, size);
}

// The slot of phonon.dll's import table that holds KERNEL32's function of this name.
void** find_import(HMODULE module, const char* name) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "KERNEL32.dll") != 0 || imp->OriginalFirstThunk == 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), name) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool write_slot(void** slot, void* value) {
    DWORD protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

} // namespace

bool acquire_zeroed_phonon_memory() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_users > 0) {
        ++g_users;
        return true;
    }
    HMODULE phonon = GetModuleHandleW(L"phonon.dll");
    if (!phonon) phonon = LoadLibraryW(L"phonon.dll"); // delay-loaded, and this is before the first call
    if (!phonon) return false;
    void** alloc = find_import(phonon, "HeapAlloc");
    void** realloc = find_import(phonon, "HeapReAlloc");
    if (!alloc || !realloc) return false;
    g_heap_alloc = reinterpret_cast<HeapAllocFn>(*alloc);
    g_heap_realloc = reinterpret_cast<HeapReAllocFn>(*realloc);
    if (!write_slot(alloc, reinterpret_cast<void*>(&zeroed_alloc))) return false;
    if (!write_slot(realloc, reinterpret_cast<void*>(&zeroed_realloc))) {
        write_slot(alloc, reinterpret_cast<void*>(g_heap_alloc));
        return false;
    }
    g_alloc_slot = alloc;
    g_realloc_slot = realloc;
    g_users = 1;
    return true;
}

void release_zeroed_phonon_memory() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_users == 0 || --g_users > 0) return;
    // Not if somebody else hooked it on top of this.
    if (*g_alloc_slot == reinterpret_cast<void*>(&zeroed_alloc)) write_slot(g_alloc_slot, reinterpret_cast<void*>(g_heap_alloc));
    if (*g_realloc_slot == reinterpret_cast<void*>(&zeroed_realloc)) write_slot(g_realloc_slot, reinterpret_cast<void*>(g_heap_realloc));
}

} // namespace sa
