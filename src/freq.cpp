/**
 * @file freq.cpp
 * @brief High-performance single-threaded word frequency counter.
 *
 * Usage: ./freq [input_file] [output_file]
 *
 * Words are sequences of latin letters a-zA-Z, lowercased.
 * Any other character is treated as a word separator.
 * Output is sorted by frequency descending, then alphabetically ascending.
 * Format: "<count> <word>\n" per line.
 *
 * Platform I/O:
 *   Linux/macOS — mmap + madvise(SEQUENTIAL), zero-copy read
 *   Windows     — CreateFile + ReadFile into a heap buffer
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#if defined(_WIN32)
    #define FREQ_PLATFORM_WINDOWS
#else
    #define FREQ_PLATFORM_POSIX
#endif

#if defined(FREQ_PLATFORM_POSIX)
    #include <cerrno>
    #include <cstring>
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#elif defined(FREQ_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Compile-time lookup table: byte → lowercase latin letter, or 0 (separator)
//  std::tolower() is locale-dependent and can be slow, so we use a precomputed lookup table instead.
//  std::isalpha() is also locale-dependent and can be slow, so we treat any non-latin letter as a separator.
//  This allows parsing the input in one pass with O(1) char classification and lowercasing.
//  only ASCII letters are supported, non-ASCII bytes are treated as separators, which is fine for the task.
// ---------------------------------------------------------------------------
static constexpr auto buildCharTable() noexcept
{
    std::array<unsigned char, 256> t{};

    // filling 'a'..'z' → 'a'..'z'
    for (int c = 'a'; c <= 'z'; ++c)
    {
        t[static_cast<unsigned char>(c)] = static_cast<unsigned char>(c);
    }

    // filling 'A'..'Z' → 'a'..'z'
    for (int c = 'A'; c <= 'Z'; ++c)
    {
        t[static_cast<unsigned char>(c)] = static_cast<unsigned char>(c - 'A' + 'a');
    }

    // all other bytes are 0 (separator)
    return t;
}

static constexpr auto kCharTable = buildCharTable();

// ---------------------------------------------------------------------------
// InputBuffer — RAII wrapper around the raw file bytes.
//  On POSIX  : mmap + madvise; destructor calls munmap.
//  On Windows: heap buffer filled by ReadFile; destructor calls delete[].
// ---------------------------------------------------------------------------
struct InputBuffer
{
    const char* data = nullptr;
    std::size_t size = 0;

#if defined(FREQ_PLATFORM_POSIX)

    explicit InputBuffer(const char* path)
    {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0)
        {
            throw std::runtime_error(std::string("Cannot open input file: ") + std::strerror(errno));
        }

        // Get file info to determine size
        struct stat st{};
        if (::fstat(fd, &st) < 0)
        {
            ::close(fd);
            throw std::runtime_error(std::string("fstat failed: ") + std::strerror(errno));
        }

        size = static_cast<std::size_t>(st.st_size);
        if (size == 0)
        {
            ::close(fd);
            return;
        }

        // Map the whole file into memory. The task says data fits in RAM, so we can do it in one call.
        //  PROT_READ: read-only access, we don't need write permissions.
        //  MAP_PRIVATE: changes are private to this process, we won't write back to the file,
        //   and it allows copy-on-write (COW) optimizations.
        void* ptr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);

        if (ptr == MAP_FAILED)
        {
            throw std::runtime_error(std::string("mmap failed: ") + std::strerror(errno));
        }

        // read mmap memory sequentially, hint to OS for better performance
        ::madvise(ptr, size, MADV_SEQUENTIAL);
        data = static_cast<const char*>(ptr);
    }

    ~InputBuffer()
    {
        if (data)
        {
            // freeing mmap memory
            ::munmap(const_cast<char*>(data), size);
        }
    }

#elif defined(FREQ_PLATFORM_WINDOWS)

    explicit InputBuffer(const char* path)
    {
        // A = ANSI (char*, 1-byte encoding)
        //  less overhead than W = Wide (wchar_t*, 2-byte encoding) since we only need ASCII chars.
        HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error(std::string("Cannot open input file: ") + path);
        }

        // a universal 64-bit number container in WinAPI
        //  QuadPart = 0x1122334455667788 (64-bit file size)
        //  HighPart = 0x11223344
        //  LowPart  = 0x55667788
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(hFile, &li))
        {
            CloseHandle(hFile);
            throw std::runtime_error("GetFileSizeEx failed");
        }

        // WIN API → C++ types
        size = static_cast<std::size_t>(li.QuadPart);
        if (size == 0)
        {
            CloseHandle(hFile);
            return;
        }

        // Allocate a heap buffer and read the file into it. This is more efficient than memory-mapping on Windows.
        char* buf = new char[size];
        DWORD bytesRead = 0;

        // ReadFile easier to use than CreateFileMapping + MapViewOfFile, and for large files it should be just as
        // efficient since the OS will handle paging.
        if (!ReadFile(hFile, buf, static_cast<DWORD>(size), &bytesRead, nullptr) ||
            bytesRead != static_cast<DWORD>(size))
        {
            delete[] buf;
            CloseHandle(hFile);
            throw std::runtime_error("ReadFile failed or short read");
        }

        CloseHandle(hFile);
        data = buf;
    }

    ~InputBuffer()
    {
        delete[] data;
    }

#endif

    // we don't need copying semantics for this simple RAII wrapper,
    // and deleting copy operations prevents accidental misuse.
    InputBuffer(const InputBuffer&) = delete;
    InputBuffer& operator=(const InputBuffer&) = delete;
};

// ---------------------------------------------------------------------------
// OutBuffer — accumulate output in memory, write in one fwrite call.
//  "<<" is bad for performance and doesn't allow reserving capacity, so we use a custom buffer.
// ---------------------------------------------------------------------------
struct OutBuffer
{
    std::vector<char> buf;

    // pre-allocate memory for the result
    explicit OutBuffer(std::size_t reserve)
    {
        buf.reserve(reserve);
    }

    // appends an integer as a string to the buffer, e.g. 123 → "123"
    // we could use std::to_string, but it allocates memory and is slower than this custom implementation.
    void appendInt(int v) noexcept
    {
        // int = max 10 digits (for 32-bit), plus optional '-' sign, 
        // so 12 chars is enough (safe constant upper bound)
        char tmp[12];
        int len = 0;

        do
        {
            // reverse order append: last digit first
            tmp[len++] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        while (v);

        for (int i = len - 1; i >= 0; --i)
        {
            buf.push_back(tmp[i]);
        }
    }

    // appends a single character to the buffer
    //  we could use buf.push_back(c) directly,
    //  but this method is more explicit and allows for future optimizations if needed.
    void appendChar(char c) noexcept
    {
        buf.push_back(c);
    }

    // appends a string to the buffer (one call)
    //  we could use buf.insert(buf.end(), s, s + n) directly,
    //  but this method is more explicit and allows for future optimizations if needed.
    void appendStr(const char* s, std::size_t n)
    {
        buf.insert(buf.end(), s, s + n);
    }

    // flushes the buffer to a file in one call; returns true on success
    bool flushTo(const char* path) noexcept
    {
        std::FILE* f = std::fopen(path, "wb");
        if (!f)
        {
            return false;
        }

        // write all data in one call; this is more efficient than multiple calls
        // and allows the OS to optimize disk access.
        const bool ok = (std::fwrite(buf.data(), 1, buf.size(), f) == buf.size());
        std::fclose(f);

        return ok;
    }
};

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: freq [input_file] [output_file]\n");
        return 1;
    }

    const char* inputPath = argv[1];
    const char* outputPath = argv[2];

    // --- 1. Read ---
    // RAII + abstraction over platform-specific file reading.
    InputBuffer ib(inputPath);

    // --- 2. Parse ---
    // map is slower than unordered_map, but it keeps keys sorted, so we won't need to sort later.
    std::unordered_map<std::string, int> freq;
    // pre-allocate ~1M buckets to avoid rehashing (assuming a large input file with many unique words)
    freq.reserve(1 << 20);

    // maybe use string_view?
    std::string word;
    word.reserve(64);

    const char* p = ib.data;
    const char* end = p + ib.size;

    while (p < end)
    {
        // O(1) array lookup
        unsigned char ch = kCharTable[static_cast<unsigned char>(*p)];
        if (ch)
        {
            word.clear();
            do
            {
                word.push_back(static_cast<char>(ch));
                ++p;

                if (p >= end)
                {
                    break;
                }

                ch = kCharTable[static_cast<unsigned char>(*p)];
            }
            while (ch);

            ++freq[word];
        }
        else
        {
            ++p;
        }
    }

    // --- 3. Sort: frequency desc, then alphabetical asc ---
    std::vector<std::pair<std::string, int>> words;
    words.reserve(freq.size());
    for (auto& kv : freq)
    {
        words.emplace_back(kv.first, kv.second);
    }

    std::sort(words.begin(), words.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
        {
            return a.second > b.second;
        }
        return a.first < b.first;
    });

    // --- 4. Write ---
    // heuristic preallocation (overestimation buffer sizing) + some extra space for formatting
    // 20 bytes per line is enough for counts up to 999999999 and words up to 10 chars, which should cover most cases.
    OutBuffer out(words.size() * 20 + 64);
    for (const auto& [w, cnt] : words)
    {
        out.appendInt(cnt);
        out.appendChar(' ');
        out.appendStr(w.data(), w.size());
        out.appendChar('\n');
    }

    if (!out.flushTo(outputPath))
    {
        std::fprintf(stderr, "Error: cannot write output file\n");
        return 1;
    }

    return 0;
}