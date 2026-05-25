#include "deegen_api.h"
#include "runtime_utils.h"
#include <vector>

// Easy way to index tables
inline TValue IndexTable(VM* vm, HeapPtr<TableObject> tbl, std::string_view key)
{
    UserHeapPointer<HeapString> hs = vm->CreateStringObjectFromRawString(key.data(), static_cast<uint32_t>(key.length()));

    GetByIdICInfo icInfo;
    TableObject::PrepareGetById(tbl, hs, icInfo);
    return TableObject::GetById(tbl, hs.As<void>(), icInfo);
}

// Easy way to set table values
template<typename TValueType, typename T>
inline void SetTableValue(VM* vm, HeapPtr<TableObject> tbl, std::string_view key, T value)
{
    UserHeapPointer<HeapString> hs = vm->CreateStringObjectFromRawString(key.data(), static_cast<uint32_t>(key.length()));

    PutByIdICInfo icInfo;
    TableObject::PreparePutById(tbl, hs, icInfo);
    TableObject::PutById(tbl, hs.As<void>(), TValue::Create<TValueType>(value), icInfo);
}

// Easy way to set raw TValues in tables (useful for inserting closures/functions directly)
inline void SetTableTValue(VM* vm, HeapPtr<TableObject> tbl, std::string_view key, TValue val)
{
    UserHeapPointer<HeapString> hs = vm->CreateStringObjectFromRawString(key.data(), static_cast<uint32_t>(key.length()));

    PutByIdICInfo icInfo;
    TableObject::PreparePutById(tbl, hs, icInfo);
    TableObject::PutById(tbl, hs.As<void>(), val, icInfo);
}

enum class FileHandleStatus {
    Valid,
    Closed,
    Invalid
};

// Extract a FILE* from a table if it has fp_low and fp_high. Returns status to avoid calling ThrowError outside of DEEGEN macros.
static FileHandleStatus TryExtractFileHandle(VM* vm, TValue val, FILE** outFp)
{
    if (!val.Is<tTable>()) return FileHandleStatus::Invalid;

    HeapPtr<TableObject> tbl = val.As<tTable>();
    TValue valLow = IndexTable(vm, tbl, "fp_low");
    TValue valHigh = IndexTable(vm, tbl, "fp_high");

    if (valLow.IsNil() || valHigh.IsNil()) return FileHandleStatus::Invalid;

    uint32_t low = static_cast<uint32_t>(valLow.AsInt32());
    uint32_t high = static_cast<uint32_t>(valHigh.AsInt32());
    
    if (low == 0 && high == 0)
    {
        return FileHandleStatus::Closed;
    }

    uintptr_t ptrVal = (static_cast<uintptr_t>(high) << 32) | low;
    *outFp = reinterpret_cast<FILE*>(ptrVal);
    return FileHandleStatus::Valid;
}

// Populates methods on a file table so `file:read(...)` works exactly like `io.read(file, ...)`
// We fetch the functions dynamically from the global 'io' table to avoid modifying vm.h
static void PopulateFileTableMethods(VM* vm, HeapPtr<TableObject> tbl)
{
    TValue ioTableVal = IndexTable(vm, vm->GetRootGlobalObject(), "io");
    if (ioTableVal.Is<tTable>())
    {
        HeapPtr<TableObject> ioTbl = ioTableVal.As<tTable>();
        SetTableTValue(vm, tbl, "read", IndexTable(vm, ioTbl, "read"));
        SetTableTValue(vm, tbl, "write", IndexTable(vm, ioTbl, "write"));
        SetTableTValue(vm, tbl, "close", IndexTable(vm, ioTbl, "close"));
        SetTableTValue(vm, tbl, "flush", IndexTable(vm, ioTbl, "flush"));
        SetTableTValue(vm, tbl, "lines", IndexTable(vm, ioTbl, "lines"));
    }
}

// io.close -- https://www.lua.org/manual/5.1/manual.html#pdf-io.close
//
// io.close ([file])
// Equivalent to file:close(). Without a file, closes the default output file.
//
DEEGEN_DEFINE_LIB_FUNC(io_close)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    FILE* fp = stdout;
    HeapPtr<TableObject> tbl = nullptr;

    if (GetNumArgs() > 0 && !GetArg(0).Is<tNil>())
    {
        FileHandleStatus status = TryExtractFileHandle(vm, GetArg(0), &fp);
        if (status == FileHandleStatus::Invalid)
        {
            ThrowError("bad argument #1 to 'close' (file handle expected)");
        }
        else if (status == FileHandleStatus::Closed)
        {
            ThrowError("attempt to use a closed file");
        }
        tbl = GetArg(0).As<tTable>();
    }

    int ret = fclose(fp);

    if (tbl != nullptr)
    {
        // Mark as closed
        SetTableValue<tInt32>(vm, tbl, "fp_low", 0);
        SetTableValue<tInt32>(vm, tbl, "fp_high", 0);
    }

    if (ret != 0)
    {
        int en = errno;
        Return(TValue::Create<tNil>(), TValue::Create<tString>(vm->CreateStringObjectFromRawCString(strerror(en))), TValue::Create<tInt32>(en));
    }
    else
    {
        Return(TValue::Create<tBool>(true));
    }
}

// io.flush -- https://www.lua.org/manual/5.1/manual.html#pdf-io.flush
//
// io.flush ()
// Equivalent to file:flush over the default output file.
//
DEEGEN_DEFINE_LIB_FUNC(io_flush)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    FILE* fp = stdout;

    if (GetNumArgs() > 0)
    {
        FILE* extractedFp = nullptr;
        FileHandleStatus status = TryExtractFileHandle(vm, GetArg(0), &extractedFp);
        if (status == FileHandleStatus::Invalid)
        {
            ThrowError("bad argument #1 to 'flush' (file handle expected)");
        }
        else if (status == FileHandleStatus::Closed)
        {
            ThrowError("attempt to use a closed file");
        }
        fp = extractedFp;
    }

    int ret = fflush(fp);
    if (ret != 0)
    {
        int en = errno;
        Return(TValue::Create<tNil>(), TValue::Create<tString>(vm->CreateStringObjectFromRawCString(strerror(en))), TValue::Create<tInt32>(en));
    }
    else
    {
        Return(TValue::Create<tBool>(true));
    }
}

// io.input -- https://www.lua.org/manual/5.1/manual.html#pdf-io.input
//
// io.input ([file])
DEEGEN_DEFINE_LIB_FUNC(io_input)
{
    ThrowError("Library function 'io.input' is not implemented yet!");
}

// 'buf' must be 'limit + 1' bytes long
// If -1 is returned, it means 'limit' bytes have been read but end-of-line is still not reached.
// If -2 is returned, it means the file hit EOF without reading in any bytes
// Otherwise, returns the length of the line, minus the '\n' if it exists.
//
static size_t WARN_UNUSED TryReadLineOnce(FILE* fp, char* buf, size_t limit)
{
    buf[limit] = 1;
    char* fgetsRet = fgets(buf, static_cast<int>(limit + 1), fp);
    
    if (unlikely(fgetsRet == nullptr))
    {
        return static_cast<size_t>(-2);
    }

    if (buf[limit] != '\0')
    {
        size_t len = strlen(buf);
        assert(len < limit);
        if (len >= 1 && buf[len - 1] == '\n')
        {
            len--;
        }
        return len;
    }

    if (buf[limit - 1] == '\n')
    {
        return limit - 1;
    }

    return static_cast<size_t>(-1);
}

static HeapPtr<HeapString> NO_INLINE ReadLinesSlowPath(FILE* fp, VM* vm, char* firstChunk, size_t firstChunkLen)
{
    std::vector<std::pair<const void*, size_t>> chunkList;
    chunkList.push_back(std::make_pair(firstChunk, firstChunkLen));

    constexpr size_t x_chunkSize = 65280;
    while (true)
    {
        char* buf = new char[x_chunkSize + 1];
        size_t len = TryReadLineOnce(fp, buf, x_chunkSize);
        if (len == static_cast<size_t>(-2))
        {
            chunkList.push_back(std::make_pair(buf, 0));
            break;
        }
        if (len == static_cast<size_t>(-1))
        {
            chunkList.push_back(std::make_pair(buf, x_chunkSize));
            continue;
        }

        assert(len < x_chunkSize);
        chunkList.push_back(std::make_pair(buf, len));
        break;
    }

    HeapPtr<HeapString> result = vm->CreateStringObjectFromConcatenation(chunkList.data(), chunkList.size()).As();

    for (size_t i = 1; i < chunkList.size(); i++)
    {
        char* ptr = const_cast<char*>(reinterpret_cast<const char*>(chunkList[i].first));
        delete [] ptr;
    }

    return result;
}

DEEGEN_DEFINE_LIB_FUNC(io_lines_iter)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    constexpr size_t x_internalBufferSize = 8192;
    char internalBuf[x_internalBufferSize + 1];
    size_t len = TryReadLineOnce(stdin, internalBuf, x_internalBufferSize);
    if (len == static_cast<size_t>(-2))
    {
        Return(TValue::Create<tNil>());
    }
    if (len != static_cast<size_t>(-1))
    {
        assert(len < x_internalBufferSize);
        Return(TValue::Create<tString>(vm->CreateStringObjectFromRawString(internalBuf, static_cast<uint32_t>(len)).As()));
    }
    HeapPtr<HeapString> result = ReadLinesSlowPath(stdin, vm, internalBuf, x_internalBufferSize);
    Return(TValue::Create<tString>(result));
}

// io.lines -- https://www.lua.org/manual/5.1/manual.html#pdf-io.lines
DEEGEN_DEFINE_LIB_FUNC(io_lines)
{
    if (GetNumArgs() > 0 && !GetArg(0).Is<tNil>())
    {
        ThrowError("Library function 'io.lines' with file input is not implemented yet!");
    }
    Return(VM_GetLibFunctionObject<VM::LibFn::IoLinesIter>());
}

// io.open -- https://www.lua.org/manual/5.1/manual.html#pdf-io.open
//
// io.open (filename [, mode])
DEEGEN_DEFINE_LIB_FUNC(io_open)
{
    if (unlikely(GetNumArgs() == 0))
    {
        ThrowError("bad argument #1 to 'open' (string expected, got no value)");
    }
    if (unlikely(!GetArg(0).Is<tString>()))
    {
        ThrowError("bad argument #1 to 'open' (string expected)");
    }

    VM* vm = VM::GetActiveVMForCurrentThread();
    HeapString* hs = TranslateToRawPointer(vm, GetArg(0).As<tString>());
    
    const char* mode = "r";
    if (GetNumArgs() > 1 && !GetArg(1).Is<tNil>())
    {
        if (unlikely(!GetArg(1).Is<tString>()))
        {
            ThrowError("bad argument #2 to 'open' (string expected)");
        }
        HeapString* modeHs = TranslateToRawPointer(vm, GetArg(1).As<tString>());
        mode = reinterpret_cast<const char*>(modeHs->m_string);
    }

    FILE* fp = fopen(reinterpret_cast<const char*>(hs->m_string), mode);
    if (fp == nullptr)
    {
        int en = errno;
        Return(TValue::Create<tNil>(), TValue::Create<tString>(vm->CreateStringObjectFromRawCString(strerror(en))), TValue::Create<tInt32>(en));
    }

    HeapPtr<TableObject> tbl = TableObject::CreateEmptyTableObject(vm, 7, 0); // Need room for methods + keys
    uintptr_t ptrVal = reinterpret_cast<uintptr_t>(fp);
    uint32_t low = static_cast<uint32_t>(ptrVal & 0xFFFFFFFF);
    uint32_t high = static_cast<uint32_t>((ptrVal >> 32) & 0xFFFFFFFF);
    
    SetTableValue<tInt32>(vm, tbl, "fp_low", static_cast<int32_t>(low));
    SetTableValue<tInt32>(vm, tbl, "fp_high", static_cast<int32_t>(high));
    
    PopulateFileTableMethods(vm, tbl);

    Return(TValue::Create<tTable>(tbl));
}

// io.output -- https://www.lua.org/manual/5.1/manual.html#pdf-io.output
DEEGEN_DEFINE_LIB_FUNC(io_output)
{
    ThrowError("Library function 'io.output' is not implemented yet!");
}

// io.popen -- https://www.lua.org/manual/5.1/manual.html#pdf-io.popen
DEEGEN_DEFINE_LIB_FUNC(io_popen)
{
    ThrowError("Library function 'io.popen' is not implemented yet!");
}

// io.read -- https://www.lua.org/manual/5.1/manual.html#pdf-io.read
//
// io.read (···)
// Equivalent to io.input():read.
//
DEEGEN_DEFINE_LIB_FUNC(io_read)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    FILE* fp = stdin; // Fallback default to standard input
    uint32_t argIdx = 0;

    // Handle being called as a method (file:read)
    if (GetNumArgs() > argIdx)
    {
        FILE* extractedFp = nullptr;
        FileHandleStatus status = TryExtractFileHandle(vm, GetArg(argIdx), &extractedFp);
        
        if (status == FileHandleStatus::Closed)
        {
            ThrowError("attempt to use a closed file");
        }
        else if (status == FileHandleStatus::Valid)
        {
            fp = extractedFp;
            argIdx++;
        }
        // If status is Invalid, it's just standard input with formatting args like io.read("*a")
    }

    bool readAll = false;
    if (GetNumArgs() > argIdx && GetArg(argIdx).Is<tString>())
    {
        HeapString* fmtHs = TranslateToRawPointer(vm, GetArg(argIdx).As<tString>());
        const char* fmt = reinterpret_cast<const char*>(fmtHs->m_string);
        if (strcmp(fmt, "*a") == 0 || strcmp(fmt, "a") == 0)
        {
            readAll = true;
        }
    }

    if (readAll)
    {
        std::vector<std::pair<const void*, size_t>> chunkList;
        constexpr size_t x_chunkSize = 65280;
        while (true)
        {
            char* buf = new char[x_chunkSize];
            size_t n = fread(buf, 1, x_chunkSize, fp);
            if (n == 0)
            {
                delete[] buf;
                break;
            }
            chunkList.push_back(std::make_pair(buf, n));
            if (n < x_chunkSize)
            {
                break;
            }
        }

        if (chunkList.empty())
        {
            Return(TValue::Create<tString>(vm->CreateStringObjectFromRawCString("")));
        }
        else
        {
            HeapPtr<HeapString> result = vm->CreateStringObjectFromConcatenation(chunkList.data(), chunkList.size()).As();
            for (size_t i = 0; i < chunkList.size(); i++)
            {
                char* ptr = const_cast<char*>(reinterpret_cast<const char*>(chunkList[i].first));
                delete[] ptr;
            }
            Return(TValue::Create<tString>(result));
        }
    }
    else
    {
        constexpr size_t x_internalBufferSize = 8192;
        char internalBuf[x_internalBufferSize + 1];
        size_t len = TryReadLineOnce(fp, internalBuf, x_internalBufferSize);
        if (len == static_cast<size_t>(-2))
        {
            Return(TValue::Create<tNil>());
        }
        if (len != static_cast<size_t>(-1))
        {
            assert(len < x_internalBufferSize);
            Return(TValue::Create<tString>(vm->CreateStringObjectFromRawString(internalBuf, static_cast<uint32_t>(len)).As()));
        }
        HeapPtr<HeapString> result = ReadLinesSlowPath(fp, vm, internalBuf, x_internalBufferSize);
        Return(TValue::Create<tString>(result));
    }
}

// io.tmpfile -- https://www.lua.org/manual/5.1/manual.html#pdf-io.tmpfile
DEEGEN_DEFINE_LIB_FUNC(io_tmpfile)
{
    ThrowError("Library function 'io.tmpfile' is not implemented yet!");
}

// io.type -- https://www.lua.org/manual/5.1/manual.html#pdf-io.type
DEEGEN_DEFINE_LIB_FUNC(io_type)
{
    ThrowError("Library function 'io.type' is not implemented yet!");
}

// io.write -- https://www.lua.org/manual/5.1/manual.html#pdf-io.write
DEEGEN_DEFINE_LIB_FUNC(io_write)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    FILE* fp = vm->GetStdout();
    uint32_t argIdx = 0;

    // Handle being called as a method (file:write)
    if (GetNumArgs() > argIdx)
    {
        FILE* extractedFp = nullptr;
        FileHandleStatus status = TryExtractFileHandle(vm, GetArg(argIdx), &extractedFp);
        
        if (status == FileHandleStatus::Closed)
        {
            ThrowError("attempt to use a closed file");
        }
        else if (status == FileHandleStatus::Valid)
        {
            fp = extractedFp;
            argIdx++;
        }
    }

    size_t numElementsToPrint = GetNumArgs();
    bool success = true;
    for (uint32_t i = argIdx; i < numElementsToPrint; i++)
    {
        TValue val = GetArg(i);
#if 0
        if (val.Is<tInt32>())
        {
            char buf[x_default_tostring_buffersize_int];
            char* bufEnd = StringifyInt32UsingDefaultLuaFormattingOptions(buf /*out*/, val.As<tInt32>());
            size_t len = static_cast<size_t>(bufEnd - buf);
            size_t written = fwrite(buf, 1, len, fp);
            if (unlikely(len != written)) { success = false; break; }
        }
        else
#endif
        if (val.Is<tDouble>())
        {
            double dbl = val.As<tDouble>();
            char buf[x_default_tostring_buffersize_double];
            char* bufEnd = StringifyDoubleUsingDefaultLuaFormattingOptions(buf /*out*/, dbl);
            size_t len = static_cast<size_t>(bufEnd - buf);
            size_t written = fwrite(buf, 1, len, fp);
            if (unlikely(len != written)) { success = false; break; }
        }
        else if (val.Is<tString>())
        {
            HeapString* hs = TranslateToRawPointer(vm, val.As<tString>());
            size_t written = fwrite(hs->m_string, sizeof(char), hs->m_length /*length*/, fp);
            if (unlikely(hs->m_length != written)) { success = false; break; }
        }
        else
        {
            ThrowError("bad argument to 'write' (string expected)");
        }
    }

    if (likely(success))
    {
        Return(TValue::Create<tBool>(true));
    }
    else
    {
        int err = errno;
        const char* errstr = strerror(err);
        TValue tvErrStr = TValue::CreatePointer(vm->CreateStringObjectFromRawString(errstr, static_cast<uint32_t>(strlen(errstr))));
        Return(TValue::Create<tNil>(), tvErrStr, TValue::Create<tDouble>(err));
    }
}

DEEGEN_END_LIB_FUNC_DEFINITIONS
